#!/usr/bin/env bash
# Sign a release .apk or .aab with the BSFChat *upload* key.
#
# Deliberately a post-build step rather than androiddeployqt's own
# --sign: that flag's interface moved between Qt 6.5 and 6.8 (argv
# pair -> QT_ANDROID_KEYSTORE_* environment), and it gives us no place
# to zipalign or to verify the result. jarsigner/apksigner are stable
# across every Qt we care about.
#
# NOTHING here is committed. The keystore arrives base64-encoded in an
# environment variable, is decoded into a private temp dir, and is
# shredded on exit. No password is ever passed as an argv word — that
# would put it in `ps` output on a shared runner — so every tool is
# driven through its env-var form.
#
# Required environment:
#   ANDROID_KEYSTORE_BASE64    base64 of the upload keystore (.jks)
#   ANDROID_KEYSTORE_PASSWORD  keystore password
#   ANDROID_KEY_ALIAS          key alias inside the keystore
#   ANDROID_KEY_PASSWORD       key password (often == store password)
#
# See docs/android-release.md for how the owner generates that
# keystore and which GitHub secrets to create. The key itself must
# never be regenerated once a build signed with it has been uploaded:
# Play binds the upload certificate to the listing.
#
# Usage:
#   scripts/sign-android.sh <input.apk|input.aab> [output]
set -euo pipefail

IN="${1:?usage: sign-android.sh <input.apk|input.aab> [output]}"
OUT="${2:-}"

[ -f "$IN" ] || { echo "sign-android: no such file: $IN" >&2; exit 1; }

for v in ANDROID_KEYSTORE_BASE64 ANDROID_KEYSTORE_PASSWORD \
         ANDROID_KEY_ALIAS ANDROID_KEY_PASSWORD; do
    if [ -z "${!v:-}" ]; then
        echo "sign-android: \$$v is empty — refusing to produce an" \
             "artefact that looks signed but is not." >&2
        exit 1
    fi
done

WORK="$(mktemp -d)"
cleanup() {
    # Overwrite before unlinking: the decoded keystore is the one
    # secret on disk during this script.
    if [ -f "$WORK/upload.jks" ]; then
        dd if=/dev/urandom of="$WORK/upload.jks" \
           bs=1 count="$(wc -c < "$WORK/upload.jks" | tr -d ' ')" \
           conv=notrunc 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

printf '%s' "$ANDROID_KEYSTORE_BASE64" | base64 --decode > "$WORK/upload.jks"
if [ ! -s "$WORK/upload.jks" ]; then
    echo "sign-android: ANDROID_KEYSTORE_BASE64 did not decode to anything." \
         "Re-create the secret with 'base64 -i upload.jks' (macOS) and make" \
         "sure no newlines were mangled on paste." >&2
    exit 1
fi

# Locate the newest build-tools for apksigner/zipalign. ANDROID_HOME and
# ANDROID_SDK_ROOT are both in the wild; accept either.
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
find_build_tool() {
    local tool="$1" found
    found="$(command -v "$tool" 2>/dev/null || true)"
    if [ -n "$found" ]; then echo "$found"; return 0; fi
    [ -n "$SDK" ] || return 1
    # Highest build-tools revision present, version-sorted.
    find "$SDK/build-tools" -maxdepth 2 -name "$tool" -type f 2>/dev/null \
        | sort -V | tail -1
}

case "$IN" in
  *.apk)
    ZIPALIGN="$(find_build_tool zipalign || true)"
    APKSIGNER="$(find_build_tool apksigner || true)"
    [ -n "$APKSIGNER" ] || { echo "sign-android: apksigner not found; set ANDROID_HOME" >&2; exit 1; }
    OUT="${OUT:-${IN%.apk}-signed.apk}"

    # zipalign BEFORE apksigner, never after: apksigner's v2/v3 block
    # is computed over the aligned archive, and realigning afterwards
    # invalidates it. Play rejects unaligned APKs outright.
    STAGE="$IN"
    if [ -n "$ZIPALIGN" ]; then
        "$ZIPALIGN" -p -f 4 "$IN" "$WORK/aligned.apk"
        STAGE="$WORK/aligned.apk"
    else
        echo "sign-android: WARNING zipalign not found — signing unaligned" >&2
    fi

    "$APKSIGNER" sign \
        --ks "$WORK/upload.jks" \
        --ks-pass "env:ANDROID_KEYSTORE_PASSWORD" \
        --ks-key-alias "$ANDROID_KEY_ALIAS" \
        --key-pass "env:ANDROID_KEY_PASSWORD" \
        --out "$OUT" \
        "$STAGE"

    "$APKSIGNER" verify --print-certs "$OUT"
    ;;

  *.aab)
    # App bundles are plain jars as far as signing goes — apksigner
    # does not handle them. Play verifies the upload key from this
    # jarsigner signature, then re-signs the generated APKs with the
    # app signing key it holds.
    OUT="${OUT:-${IN%.aab}-signed.aab}"
    cp "$IN" "$OUT"
    jarsigner -keystore "$WORK/upload.jks" \
        -storepass:env ANDROID_KEYSTORE_PASSWORD \
        -keypass:env ANDROID_KEY_PASSWORD \
        -sigalg SHA256withRSA -digestalg SHA-256 \
        "$OUT" "$ANDROID_KEY_ALIAS"
    jarsigner -verify "$OUT"
    ;;

  *)
    echo "sign-android: expected a .apk or .aab, got: $IN" >&2
    exit 1
    ;;
esac

echo "sign-android: wrote $OUT"
