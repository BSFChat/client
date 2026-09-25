#!/usr/bin/env python3
"""Read-only App Store Connect provisioning helper.

WHY THIS EXISTS
---------------
The iOS CI job used to archive with `CODE_SIGN_STYLE=Automatic` plus
`xcodebuild -allowProvisioningUpdates` and an Admin App Store Connect
key. From `man xcodebuild`:

    -allowProvisioningUpdates
        Allow xcodebuild to communicate with the Apple Developer
        website. For automatically signed targets, xcodebuild will
        create and update profiles, app IDs, and CERTIFICATES. For
        manually signed targets, xcodebuild will download missing or
        updated provisioning profiles.

Every CI run starts with an empty keychain, so "automatically signed"
meant Xcode found no Apple Development identity, and an Admin key let
it mint a brand new certificate — once per run — until the account hit
Apple's per-account certificate limit and the archive died with
"Choose a certificate to revoke."

So CI no longer signs automatically, and this script is the only thing
in the pipeline that talks to Apple before the archive. It performs
GET requests and nothing else: there is no code path here that can
create, modify or revoke anything in the developer account. The
provisioning profile it installs must already exist, created by a human
in the portal. CI consumes signing assets; it never produces them.

USAGE
-----
  fetch-profile      Download the ACTIVE App Store profile for a bundle
                     id, verify it authorises the certificate we are
                     about to sign with, and install it where xcodebuild
                     looks for it. Prints `uuid=` / `name=` lines.

  list-certificates  Print every certificate on the account with its
                     SHA-1 fingerprint, so a human can tell which row in
                     the portal matches the .p12 in IOS_DIST_CERTIFICATE
                     before revoking anything.

Credentials come from the environment, never from arguments (an
argument would land in the process table and in CI logs):

  ASC_KEY_ID, ASC_ISSUER_ID, and either ASC_KEY_PATH (a .p8 file) or
  ASC_PRIVATE_KEY_BASE64.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.appstoreconnect.apple.com"

# Where xcodebuild looks for installed profiles. Xcode 16 moved the
# directory; older Xcodes still read the original one and the runner
# image's Xcode moves under us, so write both. A profile file is inert
# — an extra copy costs nothing and a missing one costs a 20-minute
# archive.
PROFILE_DIRS = (
    "~/Library/MobileDevice/Provisioning Profiles",
    "~/Library/Developer/Xcode/UserData/Provisioning Profiles",
)


def die(msg: str) -> None:
    # ::error:: makes it a GitHub Actions annotation; harmless locally.
    print(f"::error::{msg}", file=sys.stderr)
    sys.exit(1)


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def make_token() -> str:
    """Mint a short-lived ES256 JWT for the App Store Connect API.

    Hand-rolled rather than via PyJWT so the job needs no pip install
    beyond `cryptography`, which is the only part that cannot be done
    with the standard library.
    """
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:  # pragma: no cover - environment problem
        die("python package 'cryptography' is required; pip install cryptography")

    key_id = os.environ.get("ASC_KEY_ID", "")
    issuer_id = os.environ.get("ASC_ISSUER_ID", "")
    if not key_id or not issuer_id:
        die("ASC_KEY_ID and ASC_ISSUER_ID must be set")

    key_path = os.environ.get("ASC_KEY_PATH", "")
    if key_path:
        with open(key_path, "rb") as fh:
            pem = fh.read()
    elif os.environ.get("ASC_PRIVATE_KEY_BASE64"):
        pem = base64.b64decode(os.environ["ASC_PRIVATE_KEY_BASE64"])
    else:
        die("set ASC_KEY_PATH or ASC_PRIVATE_KEY_BASE64")

    private_key = serialization.load_pem_private_key(pem, password=None)

    header = {"alg": "ES256", "kid": key_id, "typ": "JWT"}
    # 20 minutes is Apple's documented ceiling; 10 leaves room for clock
    # skew on a runner and is plenty for a couple of GETs.
    payload = {
        "iss": issuer_id,
        "iat": int(time.time()),
        "exp": int(time.time()) + 600,
        "aud": "appstoreconnect-v1",
    }
    signing_input = (
        _b64url(json.dumps(header, separators=(",", ":")).encode())
        + "."
        + _b64url(json.dumps(payload, separators=(",", ":")).encode())
    )
    der = private_key.sign(signing_input.encode(), ec.ECDSA(hashes.SHA256()))
    # JWS wants the raw r||s pair, fixed width; cryptography hands back DER.
    r, s = utils.decode_dss_signature(der)
    raw = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return signing_input + "." + _b64url(raw)


def get(token: str, path: str, params: dict | None = None) -> dict:
    """The ONLY HTTP verb this script implements, deliberately."""
    url = API + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method="GET")
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:2000]
        die(f"GET {path} failed with HTTP {exc.code}: {body}")
    except urllib.error.URLError as exc:  # pragma: no cover - network
        die(f"GET {path} failed: {exc}")
    raise AssertionError("unreachable")


def sha1_of_der(der: bytes) -> str:
    """The same fingerprint `security find-identity` prints."""
    return hashlib.sha1(der).hexdigest().upper()


def cmd_list_certificates(args: argparse.Namespace) -> int:
    token = make_token()
    data = get(
        token,
        "/v1/certificates",
        {
            "limit": "200",
            "fields[certificates]": "certificateType,displayName,name,"
            "serialNumber,expirationDate,certificateContent",
        },
    )
    rows = data.get("data", [])
    if not rows:
        print("No certificates on the account.")
        return 0
    print(f"{len(rows)} certificate(s) on the account:\n")
    for row in rows:
        attrs = row.get("attributes", {})
        content = attrs.get("certificateContent") or ""
        fingerprint = sha1_of_der(base64.b64decode(content)) if content else "?"
        print(f"  type        : {attrs.get('certificateType')}")
        print(f"  name        : {attrs.get('displayName') or attrs.get('name')}")
        print(f"  serial      : {attrs.get('serialNumber')}")
        print(f"  expires     : {attrs.get('expirationDate')}")
        print(f"  sha-1       : {fingerprint}")
        print(f"  id          : {row.get('id')}")
        print()
    print(
        "Match the sha-1 against the .p12 you hold with:\n"
        "  openssl pkcs12 -in dist.p12 -nokeys -legacy "
        "| openssl x509 -noout -fingerprint -sha1 -serial -enddate"
    )
    return 0


def cmd_fetch_profile(args: argparse.Namespace) -> int:
    token = make_token()

    # List first (cheap, no profileContent), choose, then fetch the one.
    listing = get(
        token,
        "/v1/profiles",
        {
            "limit": "200",
            "include": "bundleId,certificates",
            "fields[profiles]": "name,uuid,profileState,profileType,expirationDate",
            "fields[bundleIds]": "identifier",
            "fields[certificates]": "certificateContent",
        },
    )
    bundle_of = {
        inc["id"]: inc.get("attributes", {}).get("identifier")
        for inc in listing.get("included", [])
        if inc.get("type") == "bundleIds"
    }
    sha1_of_cert_id = {}
    for inc in listing.get("included", []):
        if inc.get("type") != "certificates":
            continue
        raw = inc.get("attributes", {}).get("certificateContent")
        if raw:
            sha1_of_cert_id[inc["id"]] = sha1_of_der(base64.b64decode(raw))

    candidates = []
    for row in listing.get("data", []):
        attrs = row.get("attributes", {})
        if attrs.get("profileType") != args.profile_type:
            continue
        rel = row.get("relationships", {}).get("bundleId", {}).get("data") or {}
        if bundle_of.get(rel.get("id")) != args.bundle_id:
            continue
        if attrs.get("profileState") != "ACTIVE":
            print(
                f"note: skipping profile '{attrs.get('name')}' "
                f"(state {attrs.get('profileState')})"
            )
            continue
        candidates.append(row)

    if not candidates:
        die(
            f"No ACTIVE {args.profile_type} provisioning profile for "
            f"'{args.bundle_id}' exists in the developer account. This job "
            "deliberately cannot create one — that is the whole point of "
            "the change that introduced this script. Create it once by hand "
            "at https://developer.apple.com/account/resources/profiles/list "
            "(Distribution -> App Store Connect), attach the Apple "
            "Distribution certificate whose private key is in the "
            "IOS_DIST_CERTIFICATE secret, and re-run."
        )

    # Prefer a profile that already authorises the certificate we
    # imported. An account can hold several ACTIVE App Store profiles
    # for one bundle id — the portal makes a new one rather than editing
    # the old — and picking one that lists a different certificate would
    # fail the check below even though a usable profile existed. If none
    # match, keep them all so the failure message is the specific one.
    if args.require_sha1:
        want = args.require_sha1.upper().replace(" ", "").replace(":", "")
        matching = [
            row
            for row in candidates
            if any(
                sha1_of_cert_id.get(ref.get("id")) == want
                for ref in (
                    row.get("relationships", {}).get("certificates", {}).get("data")
                    or []
                )
            )
        ]
        if matching:
            candidates = matching

    # Longest-lived wins: if a human made a fresh one because the old was
    # about to expire, that is the one they meant.
    chosen = max(candidates, key=lambda r: r["attributes"].get("expirationDate") or "")
    profile_id = chosen["id"]

    detail = get(
        token,
        f"/v1/profiles/{profile_id}",
        {
            "include": "certificates",
            "fields[profiles]": "name,uuid,profileState,expirationDate,profileContent",
            "fields[certificates]": "certificateContent,displayName,certificateType,"
            "serialNumber,expirationDate",
        },
    )
    attrs = detail["data"]["attributes"]
    name = attrs["name"]
    uuid = attrs["uuid"]
    content_b64 = attrs.get("profileContent")
    if not content_b64:
        die(f"profile '{name}' came back with no profileContent")

    certs = [
        inc for inc in detail.get("included", []) if inc.get("type") == "certificates"
    ]
    fingerprints = {}
    for cert in certs:
        cattrs = cert.get("attributes", {})
        raw = cattrs.get("certificateContent")
        if raw:
            fingerprints[sha1_of_der(base64.b64decode(raw))] = cattrs

    print(f"Profile      : {name}")
    print(f"UUID         : {uuid}")
    print(f"Expires      : {attrs.get('expirationDate')}")
    print(f"Certificates : {len(fingerprints)}")
    for fp, cattrs in fingerprints.items():
        print(
            f"  - {cattrs.get('certificateType')} "
            f"{cattrs.get('displayName')} sha-1 {fp} "
            f"(expires {cattrs.get('expirationDate')})"
        )

    # The expensive failure this catches: a profile that exists, is
    # ACTIVE, and simply does not list the certificate we imported. With
    # automatic signing Xcode used to "fix" that by minting a new
    # certificate. Now it is a hard stop, BEFORE the twenty-minute
    # archive rather than after it.
    if args.require_sha1:
        want = args.require_sha1.upper().replace(" ", "").replace(":", "")
        if want not in fingerprints:
            die(
                f"profile '{name}' does not authorise the certificate we "
                f"imported (sha-1 {want}). Either the .p12 in "
                "IOS_DIST_CERTIFICATE is not the certificate attached to "
                "this profile, or the profile was regenerated against a "
                "different one. Fix it in the portal; CI will not mint a "
                "certificate to paper over it."
            )
        print(f"OK: profile authorises the imported certificate {want}")

    der = base64.b64decode(content_b64)
    written = []
    for directory in PROFILE_DIRS:
        target_dir = os.path.expanduser(directory)
        os.makedirs(target_dir, exist_ok=True)
        target = os.path.join(target_dir, f"{uuid}.mobileprovision")
        with open(target, "wb") as fh:
            fh.write(der)
        written.append(target)
    for path in written:
        print(f"Installed    : {path}")

    # Sanity: it has to parse as a CMS blob, or xcodebuild will reject it
    # with something far less legible than this.
    try:
        subprocess.run(
            ["security", "cms", "-D", "-i", written[0]],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        die(f"installed profile does not parse as a provisioning profile: {exc}")

    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write(f"uuid={uuid}\n")
            fh.write(f"name={name}\n")
            fh.write(f"expires={attrs.get('expirationDate')}\n")
            fh.write("available=true\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    fetch = sub.add_parser(
        "fetch-profile", help="download + install an existing App Store profile"
    )
    fetch.add_argument("--bundle-id", required=True)
    fetch.add_argument("--profile-type", default="IOS_APP_STORE")
    fetch.add_argument(
        "--require-sha1",
        default=os.environ.get("IOS_SIGNING_SHA1", ""),
        help="fail unless the profile authorises this certificate fingerprint",
    )
    fetch.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT", ""))
    fetch.set_defaults(func=cmd_fetch_profile)

    listing = sub.add_parser(
        "list-certificates", help="print every certificate on the account"
    )
    listing.set_defaults(func=cmd_list_certificates)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
