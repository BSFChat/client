#!/bin/bash
set -e

# Build OpenSSL for iOS Simulator (arm64)
# Downloads and cross-compiles OpenSSL 3.3.2

VERSION="3.3.2"
TARGET=${1:-simulator}
PREFIX="$(pwd)/deps/openssl-ios-${TARGET}"

if [ -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "OpenSSL already built at $PREFIX"
    exit 0
fi

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

echo "OpenSSL built at $PREFIX"
