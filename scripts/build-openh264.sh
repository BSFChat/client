#!/usr/bin/env bash
# Build a static openh264 (BSD-licensed H.264 encoder/decoder) and
# vendor it into client/deps/openh264-<platform>/, following the same
# prebuilt-vendored pattern as the OpenSSL deps (see
# build-openssl-android.sh). openh264 uses its own make-based build —
# not CMake — which is why it isn't pulled via FetchContent like
# libdatachannel/opus/libaom.
#
# Usage:  scripts/build-openh264.sh [macos-arm64|macos-x64|linux-x64]
#         (default: auto-detect host)
#
# Windows note: openh264 is NOT needed on Windows — Media Foundation
# ships a guaranteed software H.264 encoder/decoder MFT. The Windows
# client builds without this dep entirely.
set -euo pipefail

OPENH264_TAG="v2.6.0"
REPO="https://github.com/cisco/openh264.git"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="$(dirname "$SCRIPT_DIR")"

PLATFORM="${1:-}"
if [[ -z "$PLATFORM" ]]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)  PLATFORM=macos-arm64 ;;
        Darwin-x86_64) PLATFORM=macos-x64 ;;
        Linux-x86_64)  PLATFORM=linux-x64 ;;
        *) echo "Unsupported host $(uname -s)-$(uname -m); pass a platform arg" >&2; exit 1 ;;
    esac
fi

case "$PLATFORM" in
    macos-arm64) MAKE_ARGS=(OS=darwin ARCH=arm64) ;;
    macos-x64)   MAKE_ARGS=(OS=darwin ARCH=x86_64) ;;
    linux-x64)   MAKE_ARGS=(OS=linux ARCH=x86_64) ;;
    *) echo "Unknown platform '$PLATFORM'" >&2; exit 1 ;;
esac

OUT_DIR="$CLIENT_DIR/deps/openh264-$PLATFORM"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "== Cloning openh264 $OPENH264_TAG"
git clone --depth 1 --branch "$OPENH264_TAG" "$REPO" "$WORK_DIR/openh264"

echo "== Building static lib for $PLATFORM"
make -C "$WORK_DIR/openh264" "${MAKE_ARGS[@]}" -j"$(getconf _NPROCESSORS_ONLN)" libraries

echo "== Vendoring into $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib" "$OUT_DIR/include/wels"
cp "$WORK_DIR/openh264/libopenh264.a" "$OUT_DIR/lib/"
cp "$WORK_DIR/openh264/codec/api/wels/"*.h "$OUT_DIR/include/wels/"
echo "$OPENH264_TAG" > "$OUT_DIR/VERSION"

echo "== Done: $(ls -lh "$OUT_DIR/lib/libopenh264.a" | awk '{print $5}') static lib at $OUT_DIR"
