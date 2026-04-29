#!/usr/bin/env bash
# Create a stable self-signed code-signing identity so dev builds
# don't get a fresh cdhash every compile, which invalidates TCC
# grants (Screen Recording, Camera) on every rebuild.
#
# Creates "BSFChat Dev Signer" in the login keychain if it doesn't
# already exist. Subsequent builds sign with this identity via a
# CMake post-build step (see CMakeLists.txt: BSFCHAT_DEV_SIGN_IDENTITY).
#
# Safe to re-run — keychain lookups short-circuit if the cert is
# already there.

set -euo pipefail
IDENTITY="BSFChat Dev Signer"

if security find-certificate -c "$IDENTITY" -a login.keychain-db >/dev/null 2>&1; then
    echo "Identity '$IDENTITY' already exists in login keychain."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Minimal certificate request with codeSigning extendedKeyUsage.
cat >"$TMPDIR/cfg" <<EOF
[ req ]
default_bits       = 2048
prompt             = no
distinguished_name = dn
req_extensions     = v3_req
x509_extensions    = v3_req

[ dn ]
CN = $IDENTITY

[ v3_req ]
keyUsage         = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
basicConstraints = critical, CA:FALSE
EOF

openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
    -keyout "$TMPDIR/key.pem" -out "$TMPDIR/cert.pem" \
    -config "$TMPDIR/cfg" -extensions v3_req >/dev/null 2>&1

openssl pkcs12 -export -inkey "$TMPDIR/key.pem" -in "$TMPDIR/cert.pem" \
    -out "$TMPDIR/bundle.p12" -name "$IDENTITY" \
    -passout pass: >/dev/null 2>&1

# Import into login keychain and allow codesign to use the key
# without an access prompt on every build.
security import "$TMPDIR/bundle.p12" -P "" -A \
    -k login.keychain-db >/dev/null 2>&1

# Trust as code-signing on the local machine. User may need to
# approve this via a one-time Keychain dialog.
sudo security add-trusted-cert -d -r trustRoot -p codeSign \
    -k /Library/Keychains/System.keychain "$TMPDIR/cert.pem" 2>/dev/null || \
    echo "  (note: skipped system trust — identity will still work for codesign)"

echo ""
echo "Created '$IDENTITY' in login keychain."
echo "Reconfigure + rebuild; CMake will auto-sign with this identity"
echo "and TCC grants will persist across rebuilds."
