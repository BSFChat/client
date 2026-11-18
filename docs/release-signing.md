# Release signing for the desktop auto-updater

The in-app updater (`src/core/Updater.cpp`) downloads the installer from the
GitHub release and runs it. On Windows that installer runs elevated and
silently. TLS only proves the bytes came from GitHub, not that we published
them, so every release also carries a signed checksum manifest, and the
client refuses to install anything that does not match it (audit M-5).

## What CI publishes

The `release` job in `.github/workflows/ci.yml` writes `SHA256SUMS`:

```
# release: v0.0.49
<sha256>  BSFChat-macOS.dmg
<sha256>  BSFChat-Setup.exe
<sha256>  BSFChat-linux-x86_64.tar.gz
```

It signs that file with Ed25519 and uploads `SHA256SUMS` and
`SHA256SUMS.sig` (a raw 64-byte signature) with the other assets:

```
openssl pkeyutl -sign -rawin -inkey key.pem -in SHA256SUMS -out SHA256SUMS.sig
```

The `# release:` line ties the manifest to its tag. Without it, a correctly
signed manifest from an older release could be uploaded to a new tag and used
to push a downgrade.

The step also checks that the secret's public key appears in
`src/core/ReleaseSigningKey.h`. A release signed with a key the clients don't
trust is a release no client will install, so the step fails in that case.

## What the client checks

All of these must pass before anything runs:

1. The asset URL must be `https://github.com/BSFChat/client/releases/download/<tag>/<name>`.
   Every redirect hop must be https on `github.com`,
   `release-assets.githubusercontent.com` (where GitHub sends release
   downloads today) or `objects.githubusercontent.com`.
2. `SHA256SUMS.sig` must verify against a key in `ReleaseSigningKey.h`, and the
   `# release:` tag must equal the tag being installed.
3. The installer is hashed while it streams to disk. It only reaches its final
   path in the cache if the hash matches the entry in `SHA256SUMS`.
4. On macOS, the `.app` inside the mounted DMG must pass
   `codesign --verify --deep --strict`. It must also have the same Team ID as
   the running build, or `BSFCHAT_MACOS_TEAM_ID` if that is defined at build
   time. These checks happen before the old app quits.

If a release has no `SHA256SUMS` + `SHA256SUMS.sig`, the client treats that as
an integrity failure ("could not be verified and was not offered") and points
nowhere else: stripping the signature is as easy for an attacker as swapping
the installer, so "download it from the website" would hand users the swapped
file. The one exception is a prerelease older than `kSignaturesRequiredFrom`
(`ReleaseSigningKey.h`, currently 0.0.49-rc.1) and older than every signed
release the client can see: those predate signing, are never installed
in-app, and the client shows their GitHub release page (URL built from the
tag, not taken from the API). So from 0.0.49-rc.1 on, **every** release,
RCs included, must be signed, or clients will report it as tampered with.

Windows Authenticode is **not** checked. The CI can sign installers
(`WINDOWS_CERT_PFX_BASE64`), but no certificate is configured today, so the
installers are unsigned. Once one is, add a `WinVerifyTrust` check to
`Updater::applyWindows`.

## Setting up the secret

1. Generate a key on a trusted machine:

   ```
   mkdir -m 700 -p ~/.bsfchat-release && cd ~/.bsfchat-release
   (umask 077; openssl genpkey -algorithm ed25519 -out release-signing-ed25519.pem)
   ```

2. Print the raw public key and put it in `kReleaseSigningKeysHex` in
   `src/core/ReleaseSigningKey.h`:

   ```
   openssl pkey -in release-signing-ed25519.pem -pubout -outform DER | tail -c 32 | od -An -tx1 -v | tr -d ' \n'
   ```

3. In GitHub, under BSFChat/client → Settings → Secrets and variables →
   Actions, add a repository secret named **`RELEASE_SIGNING_KEY`**. Its value
   is the whole PEM file, including the `BEGIN`/`END` lines.
4. Keep an offline backup of the PEM. If it is lost, you will need to ship a
   client update with a new key to every user.

When the secret is missing:

- **Stable tags** (`vX.Y.Z`): the release job fails.
- **RC tags** (`vX.Y.Z-suffix`): the job warns and publishes without
  `SHA256SUMS.sig`. Clients report such an RC as failing verification (see
  above), so configure the secret before tagging.

## Rotation

A client only trusts the keys it was built with. To rotate:

1. Generate the new key and **add** its public key to
   `kReleaseSigningKeysHex`, keeping the old one.
2. Ship at least one release signed with the **old** key. That release
   carries both keys.
3. Switch `RELEASE_SIGNING_KEY` to the new PEM.
4. Remove the old public key only when no supported build depends on it.
   Users still on older builds will then need to update manually from the
   website.

If the key is compromised, do steps 1 and 3 together and remove the
compromised key in the same release. Builds that only trust the
compromised key can't be protected. Tell those users to reinstall from the
website.

## Manual verification

```
openssl pkeyutl -verify -rawin -pubin -inkey release-signing-ed25519.pub.pem \
  -in SHA256SUMS -sigfile SHA256SUMS.sig
sha256sum -c --ignore-missing SHA256SUMS
```

GNU `sha256sum` skips the `# release:` comment line.
