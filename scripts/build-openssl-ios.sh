#!/bin/bash
set -e

# Build OpenSSL for iOS Simulator (arm64)
# Downloads and cross-compiles OpenSSL.
# 3.5.7 (current LTS): 3.3.2 is EOL and was affected by CVE-2025-15467
# and CVE-2026-45447. Regenerate deps/openssl-ios-* after bumping.

VERSION="3.5.7"
TARGET=${1:-simulator}
PREFIX="$(pwd)/deps/openssl-ios-${TARGET}"

# Skip only when the existing build matches VERSION — a version-blind
# guard would keep shipping the stale OpenSSL after a bump.
if [ -f "$PREFIX/lib/libcrypto.a" ] \
   && [ "$(cat "$PREFIX/.bsfchat-openssl-version" 2>/dev/null)" = "$VERSION" ]; then
    echo "OpenSSL $VERSION already built at $PREFIX"
    exit 0
fi
rm -rf "$PREFIX"

echo "Building OpenSSL $VERSION for iOS ($TARGET)..."

mkdir -p deps && cd deps

if [ ! -d "openssl-$VERSION" ]; then
    curl -sL "https://github.com/openssl/openssl/releases/download/openssl-${VERSION}/openssl-${VERSION}.tar.gz" | tar xz
fi

cd "openssl-$VERSION"

if [ "$TARGET" = "simulator" ]; then
    ./Configure iossimulator-xcrun --prefix="$PREFIX" no-shared no-tests no-docs
else
    ./Configure ios64-xcrun --prefix="$PREFIX" no-shared no-tests no-docs
fi

make -j$(sysctl -n hw.ncpu)
make install_sw

echo "$VERSION" > "$PREFIX/.bsfchat-openssl-version"
echo "OpenSSL built at $PREFIX"
