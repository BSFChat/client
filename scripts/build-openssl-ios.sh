#!/bin/bash
set -e

# Build OpenSSL for iOS Simulator (arm64)
# Downloads and cross-compiles OpenSSL.
# 3.5.7 (current LTS): 3.3.2 is EOL and was affected by CVE-2025-15467
# and CVE-2026-45447. Regenerate deps/openssl-ios-* after bumping.

VERSION="3.5.7"
TARGET=${1:-simulator}
PREFIX="$(pwd)/deps/openssl-ios-${TARGET}"

# Deployment target: the app's floor, not the SDK's. Without an explicit
# -mios-version-min the xcrun targets build for whatever SDK the Xcode ships
# (26.x), and the 17.4 app link then emits ~900 "built for newer iOS" warnings
# and can reference symbols older devices lack. Defaults to the value in
# CMakeLists.txt (BSFCHAT_IOS_MIN_VERSION) so the two cannot drift.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${BSFCHAT_IOS_MIN_VERSION:-}" ]; then
    BSFCHAT_IOS_MIN_VERSION=$(sed -n 's/^set(BSFCHAT_IOS_MIN_VERSION "\([0-9.]*\)").*/\1/p' \
        "$SCRIPT_DIR/../CMakeLists.txt" 2>/dev/null | head -1)
fi
MIN_VERSION="${BSFCHAT_IOS_MIN_VERSION:-17.4}"
if [ "$TARGET" = "simulator" ]; then
    MIN_FLAG="-mios-simulator-version-min=$MIN_VERSION"
else
    MIN_FLAG="-mios-version-min=$MIN_VERSION"
fi
export IPHONEOS_DEPLOYMENT_TARGET="$MIN_VERSION"
STAMP="$VERSION ios$MIN_VERSION $(xcrun --show-sdk-version --sdk iphoneos 2>/dev/null || echo unknown-sdk)"

# Skip only when the existing build matches VERSION, the deployment target
# and the SDK: a blind guard would keep shipping a stale or mis-targeted build.
if [ -f "$PREFIX/lib/libcrypto.a" ] \
   && [ "$(cat "$PREFIX/.bsfchat-openssl-version" 2>/dev/null)" = "$STAMP" ]; then
    echo "OpenSSL $STAMP already built at $PREFIX"
    exit 0
fi
rm -rf "$PREFIX"

echo "Building OpenSSL $VERSION for iOS ($TARGET, minimum iOS $MIN_VERSION)..."

mkdir -p deps && cd deps

if [ ! -d "openssl-$VERSION" ]; then
    curl -sL "https://github.com/openssl/openssl/releases/download/openssl-${VERSION}/openssl-${VERSION}.tar.gz" | tar xz
fi

cd "openssl-$VERSION"

if [ "$TARGET" = "simulator" ]; then
    ./Configure iossimulator-xcrun --prefix="$PREFIX" no-shared no-tests no-docs "$MIN_FLAG"
else
    ./Configure ios64-xcrun --prefix="$PREFIX" no-shared no-tests no-docs "$MIN_FLAG"
fi

# A previous configure in the same source tree may have left objects built
# for another target or deployment version.
make clean >/dev/null 2>&1 || true
make -j$(sysctl -n hw.ncpu)
make install_sw

# Fail rather than ship a library whose objects claim a newer minimum.
if command -v otool >/dev/null; then
    BAD=$(otool -l "$PREFIX/lib/libcrypto.a" | awk '/LC_BUILD_VERSION/{f=1} f&&$1=="minos"{print $2; f=0}' \
          | sort -u | grep -vx "$MIN_VERSION" || true)
    if [ -n "$BAD" ]; then
        echo "libcrypto.a has objects with minos $BAD, expected $MIN_VERSION" >&2
        exit 1
    fi
fi

echo "$STAMP" > "$PREFIX/.bsfchat-openssl-version"
echo "OpenSSL built at $PREFIX"
