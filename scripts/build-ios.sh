#!/bin/bash
set -e

# BSFChat iOS build script
# Usage: ./scripts/build-ios.sh [device|simulator]

TARGET=${1:-simulator}
QT_IOS=~/Qt/6.8.0/ios
QT_HOST=~/Qt/6.8.0/macos
BUILD_DIR=build-ios-${TARGET}
OPENSSL_DIR="$(pwd)/deps/openssl-ios-${TARGET}"

if [ ! -d "$QT_IOS" ]; then
    echo "Qt for iOS not found at $QT_IOS"
    echo "Install with: aqt install-qt mac ios 6.8.0 -m qtmultimedia --outputdir ~/Qt"
    exit 1
fi

# Build OpenSSL for iOS if not already done
if [ ! -f "$OPENSSL_DIR/lib/libcrypto.a" ]; then
    echo "Building OpenSSL for iOS ($TARGET)..."
    ./scripts/build-openssl-ios.sh $TARGET
fi

echo "Building BSFChat for iOS ($TARGET)..."

if [ "$TARGET" = "simulator" ]; then
    OPENSSL_ROOT_DIR="$OPENSSL_DIR" \
    $QT_IOS/bin/qt-cmake -B $BUILD_DIR \
        -G Xcode \
        -DCMAKE_OSX_ARCHITECTURES="arm64" \
        -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DCMAKE_BUILD_TYPE=Release \
        -DGAMECHAT_CLIENT_BUILD_TESTS=OFF \
        -DOPENSSL_ROOT_DIR="$OPENSSL_DIR" \
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_DIR/include" \
        -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_DIR/lib/libcrypto.a" \
        -DOPENSSL_SSL_LIBRARY="$OPENSSL_DIR/lib/libssl.a" \
        -DQT_HOST_PATH="$QT_HOST" \
        -DMACOSX_BUNDLE_INFO_PLIST="$(pwd)/ios/Info.plist" \
        -Wno-dev
else
    OPENSSL_ROOT_DIR="$OPENSSL_DIR" \
    $QT_IOS/bin/qt-cmake -B $BUILD_DIR \
        -G Xcode \
        -DCMAKE_OSX_ARCHITECTURES="arm64" \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_BUILD_TYPE=Release \
        -DGAMECHAT_CLIENT_BUILD_TESTS=OFF \
        -DOPENSSL_ROOT_DIR="$OPENSSL_DIR" \
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_DIR/include" \
        -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_DIR/lib/libcrypto.a" \
        -DOPENSSL_SSL_LIBRARY="$OPENSSL_DIR/lib/libssl.a" \
        -DQT_HOST_PATH="$QT_HOST" \
        -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${DEVELOPMENT_TEAM:-}" \
        -DMACOSX_BUNDLE_INFO_PLIST="$(pwd)/ios/Info.plist" \
        -Wno-dev
fi

echo ""
echo "Xcode project generated at: $BUILD_DIR/"
echo ""
echo "To open in Xcode:"
echo "  open $BUILD_DIR/bsfchat-app.xcodeproj"
echo ""
echo "In Xcode: select an iOS Simulator target and press Run (Cmd+R)"
