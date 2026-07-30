#!/usr/bin/env bash
# Vendor the prebuilt LiveKit C++ client SDK into
# client/deps/livekit-<platform>-<version>/, following the same
# prebuilt-vendored pattern as openh264 and the OpenSSL deps (see
# build-openh264.sh, build-openssl-android.sh).
#
# Why prebuilts rather than FetchContent: livekit/client-sdk-cpp wraps a
# Rust core (livekit-ffi) via a git submodule, so a source build needs a
# stable Rust toolchain plus git-lfs on the critical path of every build.
# Upstream publishes per-triple release archives; we take those.
#
# Usage:  scripts/fetch-livekit-sdk.sh [macos-arm64|macos-x64|linux-x64|linux-arm64|windows-x64]
#         (default: auto-detect host)
#
# Android/iOS: NOT supported by this SDK. There are no mobile release
# assets and docs/building.md lists Linux/macOS/Windows only. Mobile
# voice stays on the libdatachannel mesh path — do not try to point this
# script at an Android triple.
set -euo pipefail

# Pinned version. Bump deliberately: the SDK is young (v1.0.0 shipped
# 2026-06-01) and the README records breaking API changes at v1.0.0, so
# a bump is an API-review event, not a routine dependency refresh.
LIVEKIT_VERSION="1.5.0"
REPO="livekit/client-sdk-cpp"

# SHA-256 of each pinned release asset.
#
# Upstream publishes NO checksum file alongside the archives — verified
# against the v1.5.0 release asset list, which contains exactly the five
# archives and nothing else. These digests were therefore computed from
# the archives as downloaded on 2026-07-30 and pinned here. That means
# they attest "the same bytes everyone else got on this date", not an
# upstream-signed claim. Treat a mismatch as "investigate", not
# "corrupt download": upstream re-uploading an asset in place would look
# identical to tampering, which is exactly why the digest is pinned.
sha256_for() {
    case "$1" in
        macos-arm64)  echo "ffa9396a009488c3d050bccb4dd07175c42ee37f484a53969d97175b32291c54" ;;
        macos-x64)    echo "377a8ade01ef39740c2949899fe0e3c61e7c8a18676db022929c7fbf991d715e" ;;
        linux-x64)    echo "4e199577d2ec318d210cf04b65e24e635219c35336b0eaed3b9adfc32fcaf185" ;;
        linux-arm64)  echo "971a4a3e6ad62611d6a4827d23c12b8fa3b9e751791f17771c8ea57cf80f1860" ;;
        windows-x64)  echo "ff5c6d1f5a96bf0df11208aa1b5acabb50e65f4d28e41d67521e2a67dc3e0f04" ;;
        *) return 1 ;;
    esac
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="$(dirname "$SCRIPT_DIR")"

PLATFORM="${1:-}"
if [[ -z "$PLATFORM" ]]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)   PLATFORM=macos-arm64 ;;
        Darwin-x86_64)  PLATFORM=macos-x64 ;;
        Linux-x86_64)   PLATFORM=linux-x64 ;;
        Linux-aarch64)  PLATFORM=linux-arm64 ;;
        MINGW*|MSYS*|CYGWIN*) PLATFORM=windows-x64 ;;
        *) echo "Unsupported host $(uname -s)-$(uname -m); pass a platform arg" >&2; exit 1 ;;
    esac
fi

case "$PLATFORM" in
    windows-x64) EXT="zip" ;;
    macos-arm64|macos-x64|linux-x64|linux-arm64) EXT="tar.gz" ;;
    *) echo "Unknown platform '$PLATFORM'" >&2
       echo "Valid: macos-arm64 macos-x64 linux-x64 linux-arm64 windows-x64" >&2
       exit 1 ;;
esac

EXPECTED_SHA="$(sha256_for "$PLATFORM")"
ASSET="livekit-sdk-${PLATFORM}-${LIVEKIT_VERSION}.${EXT}"
URL="https://github.com/${REPO}/releases/download/v${LIVEKIT_VERSION}/${ASSET}"
OUT_DIR="$CLIENT_DIR/deps/livekit-${PLATFORM}-${LIVEKIT_VERSION}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "== Downloading $ASSET"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$WORK_DIR/$ASSET" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$WORK_DIR/$ASSET" "$URL"
else
    echo "Neither curl nor wget available" >&2; exit 1
fi

echo "== Verifying SHA-256"
if command -v shasum >/dev/null 2>&1; then
    ACTUAL_SHA="$(shasum -a 256 "$WORK_DIR/$ASSET" | awk '{print $1}')"
else
    ACTUAL_SHA="$(sha256sum "$WORK_DIR/$ASSET" | awk '{print $1}')"
fi
if [[ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]]; then
    echo "SHA-256 MISMATCH for $ASSET" >&2
    echo "  expected: $EXPECTED_SHA" >&2
    echo "  actual:   $ACTUAL_SHA" >&2
    echo "Refusing to vendor. Either upstream replaced the asset in place" >&2
    echo "(check the release page) or the download was tampered with." >&2
    exit 1
fi
echo "   ok  $ACTUAL_SHA"

echo "== Extracting"
mkdir -p "$WORK_DIR/x"
if [[ "$EXT" == "zip" ]]; then
    unzip -q "$WORK_DIR/$ASSET" -d "$WORK_DIR/x"
else
    tar xzf "$WORK_DIR/$ASSET" -C "$WORK_DIR/x"
fi

# Archives contain a single top-level livekit-sdk-<platform>-<version>/ dir.
SRC="$WORK_DIR/x/livekit-sdk-${PLATFORM}-${LIVEKIT_VERSION}"
if [[ ! -d "$SRC" ]]; then
    echo "Unexpected archive layout: $SRC missing. Contents:" >&2
    find "$WORK_DIR/x" -maxdepth 2 >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$(dirname "$OUT_DIR")"
mv "$SRC" "$OUT_DIR"

# Apache-2.0 requires attribution, and the release archives ship NO
# LICENSE or NOTICE file (verified across all five v1.5.0 assets). Drop
# a pointer in so the vendored tree is not silently unattributed; the
# client's third-party notices need a LiveKit entry before any release
# build that links this ships.
cat > "$OUT_DIR/LICENSE-NOTE.txt" <<EOF
LiveKit C++ client SDK ${LIVEKIT_VERSION} — Apache License 2.0
Source: https://github.com/${REPO}
Licence text: https://github.com/${REPO}/blob/v${LIVEKIT_VERSION}/LICENSE

The upstream release archives contain no LICENSE or NOTICE file. This
note exists so the vendored tree is not unattributed. The bundled Rust
core (livekit_ffi) links libwebrtc and other transitive dependencies
whose licences are NOT enumerated by upstream's DEPENDENCIES.md — a
full transitive licence audit is still outstanding and must happen
before a release build that ships these binaries.
EOF

echo
echo "== Vendored to $OUT_DIR"
echo "   Configure with: -DBSFCHAT_ENABLE_LIVEKIT=ON"
if [[ -f "$OUT_DIR/share/livekit/build-info.json" ]]; then
    echo "   build-info.json:"
    sed 's/^/     /' "$OUT_DIR/share/livekit/build-info.json"
fi
