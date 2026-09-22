#!/bin/bash
set -e

# BSFChat iOS build script — the hand-driven equivalent of the `ios` job
# in .github/workflows/ci.yml. Generates an Xcode project; it does not
# archive or sign. See docs/ios-release.md for the App Store path.
#
# Usage: ./scripts/build-ios.sh [device|simulator]

TARGET=${1:-simulator}

# 6.10.3, in lockstep with .github/workflows/ci.yml. It used to say
# 6.8.0, which is a problem beyond drift: QQuickWindow.safeAreaMargins
# and the QML SafeArea attached property — the fix for the iPhone notch
# and home indicator — only exist from Qt 6.9, so a 6.8 toolchain
# cannot even express the layout the mobile UI needs.
QT_VERSION=6.10.3
QT_IOS=~/Qt/${QT_VERSION}/ios
QT_HOST=~/Qt/${QT_VERSION}/macos
BUILD_DIR=build-ios-${TARGET}
OPENSSL_DIR="$(pwd)/deps/openssl-ios-${TARGET}"

# Voice is OFF by default on iOS (CMakeLists.txt): libdatachannel, opus
# and the capture paths are not ported. Pass BSFCHAT_ENABLE_VOICE=ON to
# work on that port — which is also the only case that needs the
# cross-built OpenSSL, since nothing else in the client links it.
ENABLE_VOICE=${BSFCHAT_ENABLE_VOICE:-OFF}

if [ ! -d "$QT_IOS" ]; then
    echo "Qt for iOS not found at $QT_IOS"
    echo "Install with: aqt install-qt mac ios ${QT_VERSION} -m qtmultimedia --outputdir ~/Qt"
    exit 1
fi
if [ ! -d "$QT_HOST" ]; then
    echo "Qt for macOS (host tools: moc/rcc/qmlimportscanner) not found at $QT_HOST"
    echo "Install with: aqt install-qt mac desktop ${QT_VERSION} -m qtmultimedia --outputdir ~/Qt"
    exit 1
fi

COMMON_ARGS=(
    -B "$BUILD_DIR"
    -G Xcode
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_BUILD_TYPE=Release
    -DGAMECHAT_CLIENT_BUILD_TESTS=OFF
    -DBSFCHAT_ENABLE_VOICE="$ENABLE_VOICE"
    -DQT_HOST_PATH="$QT_HOST"
    -Wno-dev
)

if [ "$ENABLE_VOICE" = "ON" ]; then
    if [ ! -f "$OPENSSL_DIR/lib/libcrypto.a" ]; then
        echo "Building OpenSSL for iOS ($TARGET)..."
        ./scripts/build-openssl-ios.sh "$TARGET"
    fi
    COMMON_ARGS+=(
        -DOPENSSL_ROOT_DIR="$OPENSSL_DIR"
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_DIR/include"
        -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_DIR/lib/libcrypto.a"
        -DOPENSSL_SSL_LIBRARY="$OPENSSL_DIR/lib/libssl.a"
    )
fi

# NOTE: no -DMACOSX_BUNDLE_INFO_PLIST here any more. It never worked —
# it is a cache variable, and CMakeLists.txt sets the same name as a
# TARGET PROPERTY, which wins. Worse, that property was set inside a
# bare `if(APPLE)`, so every iOS build silently shipped the *macOS*
# plist. CMakeLists.txt now has a real `if(IOS)` branch that configures
# ios/Info.plist.in; leave the plist to it.

echo "Building BSFChat for iOS ($TARGET, voice=$ENABLE_VOICE)..."

if [ "$TARGET" = "simulator" ]; then
    OPENSSL_ROOT_DIR="$OPENSSL_DIR" \
    $QT_IOS/bin/qt-cmake "${COMMON_ARGS[@]}" \
        -DCMAKE_OSX_SYSROOT=iphonesimulator
else
    OPENSSL_ROOT_DIR="$OPENSSL_DIR" \
    $QT_IOS/bin/qt-cmake "${COMMON_ARGS[@]}" \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${DEVELOPMENT_TEAM:-}"
fi

echo ""
echo "Xcode project generated at: $BUILD_DIR/"
echo ""
echo "To open in Xcode:"
echo "  open $BUILD_DIR/bsfchat-app.xcodeproj"
echo ""
echo "In Xcode: select an iOS Simulator target and press Run (Cmd+R)"
