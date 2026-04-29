cmake_minimum_required(VERSION 3.20...3.31)
project(bsfchat-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GAMECHAT_CLIENT_BUILD_TESTS "Build client tests" ON)
# Voice defaults OFF on iOS for the MVP mobile build — libdatachannel
# + opus + audio capture paths need mobile-specific plumbing that's
# on the roadmap but not done yet. Enable explicitly once that work
# lands: cmake -DBSFCHAT_ENABLE_VOICE=ON.
if(IOS)
    option(BSFCHAT_ENABLE_VOICE "Enable voice chat (requires libdatachannel + opus)" OFF)
else()
    # Android: voice builds against our NDK-compiled OpenSSL (see
    # deps/openssl-android-arm64). libdatachannel + opus are pulled
    # via FetchContent and cross-compile cleanly through the NDK
    # toolchain. Mic capture uses Qt Multimedia's QAudioSource which
    # maps to Android's AudioRecord — needs RECORD_AUDIO runtime
    # permission (requested at first voice-join via Haptics cousin
    # AudioPermission, wired below).
    option(BSFCHAT_ENABLE_VOICE "Enable voice chat (requires libdatachannel + opus)" ON)
endif()

# On Android, point FetchContent'd libdatachannel at the OpenSSL we
# cross-built in scripts/build-openssl-android.sh. Without this hint
# libdatachannel's find_package(OpenSSL) falls back to the host
# system's OpenSSL (wrong ABI) and linking fails.
if(ANDROID AND BSFCHAT_ENABLE_VOICE)
    set(OPENSSL_ROOT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/openssl-android-arm64"
        CACHE PATH "Android OpenSSL root" FORCE)
    set(OPENSSL_INCLUDE_DIR
        "${OPENSSL_ROOT_DIR}/include" CACHE PATH "" FORCE)
    set(OPENSSL_SSL_LIBRARY
        "${OPENSSL_ROOT_DIR}/lib/libssl_3.so" CACHE FILEPATH "" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY
        "${OPENSSL_ROOT_DIR}/lib/libcrypto_3.so" CACHE FILEPATH "" FORCE)
endif()

include(cmake/Dependencies.cmake)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Network Qml Quick QuickControls2 QuickDialogs2 Sql Multimedia Svg Widgets)
qt_standard_project_setup()
qt_policy(SET QTP0001 NEW)
# QTP0004 was introduced in Qt 6.6 — guard on Qt version so older
# Qt installs (e.g. 6.5 on Android) don't error out.
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.6")
    qt_policy(SET QTP0004 OLD)
endif()

qt_add_executable(bsfchat-app
    src/main.cpp
    src/core/App.cpp
    src/core/NotificationManager.cpp
    src/core/Settings.cpp
    src/core/TintedIconProvider.cpp
    src/core/MediaDownloader.cpp
    src/core/Haptics.cpp
    src/core/AndroidPermissions.cpp
    src/core/AndroidNotifier.cpp
    src/core/UrlHandler.cpp
    src/identity/IdentityClient.cpp
    src/identity/IdentityApiClient.cpp
    src/net/MatrixClient.cpp
    src/net/SyncLoop.cpp
    src/net/ServerConnection.cpp
    src/net/ServerManager.cpp
    src/model/ServerListModel.cpp
    src/model/RoomListModel.cpp
    src/model/MessageModel.cpp
    src/model/MemberListModel.cpp
    src/store/LocalCache.cpp
    src/util/MarkdownParser.cpp
    # NotificationSounds is used by NotificationManager for chat
    # chimes — always needed, not voice-gated. SoundGenerator is
    # its only dep (a tiny pure-DSP helper).
    src/voice/NotificationSounds.cpp
    src/voice/SoundGenerator.cpp
)

if(BSFCHAT_ENABLE_VOICE)
    target_sources(bsfchat-app PRIVATE
        src/voice/VoiceEngine.cpp
        src/voice/PeerConnectionManager.cpp
        src/voice/AudioEngine.cpp
        src/voice/AudioMixer.cpp
        # SoundGenerator + NotificationSounds moved out of this
        # block — they're needed for chat chimes on mobile too.
        # Android: toggle MODE_IN_COMMUNICATION + speakerphone on
        # voice start, restore on stop. No-op on other platforms so
        # the source is unconditionally compiled.
        src/voice/AndroidAudioRouting.cpp
        src/voice/AndroidScreenShareController.cpp
        src/voice/ScreenShareController.cpp
        src/voice/CameraController.cpp
        # macOS-only (not iOS): ScreenCaptureKit / AVCaptureDevice
        # device paths are desktop-shape and rely on Qt Multimedia
        # features (QVideoSink pipelines) that need rework for iOS.
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacScreenCapturer.mm>
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacCameraPermission.mm>
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacCameraCapturer.mm>
    )
    target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_VOICE_ENABLED)
endif()

# Android: point Qt at our custom package source dir which carries a
# patched AndroidManifest.xml with softInputMode=adjustResize so the
# on-screen keyboard shrinks the window rather than covering the
# composer.
if(ANDROID)
    set_property(TARGET bsfchat-app PROPERTY
        QT_ANDROID_PACKAGE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/android")

    # Bundle OpenSSL with the APK. Qt for Android ships without SSL, so
    # HTTPS requests (OIDC token exchange, the Matrix/JSON client,
    # media fetches) all fail silently at the TLS layer unless libssl
    # and libcrypto are packaged. androiddeployqt reads this property
    # and copies the .so files into lib/<abi>/ inside the APK.
    set(BSFCHAT_OPENSSL_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/openssl-android-arm64/lib")
    if(EXISTS "${BSFCHAT_OPENSSL_DIR}/libssl_3.so"
       AND EXISTS "${BSFCHAT_OPENSSL_DIR}/libcrypto_3.so")
        set_property(TARGET bsfchat-app PROPERTY
            QT_ANDROID_EXTRA_LIBS
            "${BSFCHAT_OPENSSL_DIR}/libssl_3.so"
            "${BSFCHAT_OPENSSL_DIR}/libcrypto_3.so")
    else()
        message(WARNING "OpenSSL for Android not found at "
            "${BSFCHAT_OPENSSL_DIR}. Run scripts/build-openssl-android.sh "
            "or HTTPS requests will fail at runtime.")
    endif()
endif()

# Ensure macOS .app bundle is created.
#
# Bundle ID is overridable — the default `com.bsfchat.app.dev` keeps
# local Debug builds out of Launch Services' way when a release
# BSFChat.app is also installed (otherwise `open` picks the installed
# one and the dev build is unreachable from the Dock / ⌘Q → reopen
# flow). CI + release builds pass -DBSFCHAT_BUNDLE_ID=com.bsfchat.app
# to restore the canonical identity.
if(NOT DEFINED BSFCHAT_BUNDLE_ID)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(BSFCHAT_BUNDLE_ID "com.bsfchat.app")
    else()
        set(BSFCHAT_BUNDLE_ID "com.bsfchat.app.dev")
    endif()
endif()
set_target_properties(bsfchat-app PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BSFChat"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${BSFCHAT_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.0.22"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.0.22"
    MACOSX_BUNDLE_INFO_STRING "BSFChat"
    MACOSX_BUNDLE_ICON_FILE "BSFChat.icns"
    WIN32_EXECUTABLE TRUE
)

# Bundle the brand .icns into Contents/Resources/ so the value of
# MACOSX_BUNDLE_ICON_FILE actually resolves at runtime. Without
# this, Finder shows the generic Qt icon even though Info.plist
# names the right file.
if(APPLE)
    set(BSFCHAT_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/branding/BSFChat.icns")
    if(EXISTS "${BSFCHAT_ICNS}")
        target_sources(bsfchat-app PRIVATE "${BSFCHAT_ICNS}")
        set_source_files_properties("${BSFCHAT_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    endif()
endif()

# Windows resource file — embeds the .ico into the .exe so Explorer,
# the taskbar, and the file dialog all show the brand mark. Without
# a .rc the exe falls back to Windows' default executable glyph.
if(WIN32)
    set(BSFCHAT_RC "${CMAKE_CURRENT_BINARY_DIR}/bsfchat-app.rc")
    file(WRITE "${BSFCHAT_RC}"
        "IDI_ICON1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/branding/BSFChat.ico\"\n")
    target_sources(bsfchat-app PRIVATE "${BSFCHAT_RC}")
endif()

if(APPLE)
    # CMake's generated Info.plist only covers the canonical MACOSX_BUNDLE_*
    # keys. Inject the TCC usage descriptions we need after the bundle has
    # been written — without NSMicrophoneUsageDescription, macOS denies mic
    # access silently with no prompt, so voice chat opens the input device
    # but only captures silence.
    add_custom_command(TARGET bsfchat-app POST_BUILD
        # CMake writes the plist read-only; plutil -replace refuses to edit
        # it without write permission. Make it writable first.
        COMMAND /bin/chmod u+w
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/bin/plutil -replace NSMicrophoneUsageDescription
                -string "BSFChat needs microphone access so you can talk in voice channels."
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/bin/plutil -replace NSCameraUsageDescription
                -string "BSFChat needs camera access to share your webcam in voice channels."
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        # Screen Recording permission is gated by TCC without a
        # dedicated string — add it so macOS prompts on first capture
        # attempt instead of silently refusing.
        COMMAND /usr/bin/plutil -replace NSScreenCaptureUsageDescription
                -string "BSFChat needs screen-recording access so you can share your screen in voice channels."
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        # Register `bsfchat://` as a URL scheme the app handles. Launch
        # Services reads CFBundleURLTypes on install and routes activation
        # to our QFileOpenEvent filter (see src/core/UrlHandler.cpp).
        # PlistBuddy is happier with CMake VERBATIM than `plutil -json`,
        # which gets its argument mangled by the quoting pass. Each Delete
        # tolerates absence (|| true) so this is idempotent across builds.
        COMMAND /bin/sh -c "/usr/libexec/PlistBuddy -c 'Delete :CFBundleURLTypes' \"$0\" || true" "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/libexec/PlistBuddy -c "Add :CFBundleURLTypes array"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/libexec/PlistBuddy -c "Add :CFBundleURLTypes:0 dict"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/libexec/PlistBuddy -c "Add :CFBundleURLTypes:0:CFBundleURLName string com.bsfchat.url"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/libexec/PlistBuddy -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes array"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        COMMAND /usr/libexec/PlistBuddy -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes:0 string bsfchat"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist"
        VERBATIM
    )
endif()

set_source_files_properties(qml/theme/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(bsfchat-app
    URI BSFChat
    VERSION 1.0
    # Icon.qml uses Qt5Compat.GraphicalEffects' ColorOverlay — declare
    # the dependency explicitly so Qt's Android deployment picks up
    # the plugin and its qmldir registers at launch. Without this,
    # the Qt5Compat module is bundled in the APK but not loaded
    # (icons silently render as transparent/black).
    IMPORTS Qt5Compat.GraphicalEffects
    QML_FILES
        qml/main.qml
        qml/mobile/MobileMain.qml
        qml/components/ServerSidebar.qml
        qml/components/ChannelList.qml
        qml/components/MessageView.qml
        qml/components/MessageBubble.qml
        qml/components/MessageInput.qml
        qml/components/EmojiPicker.qml
        qml/components/VoiceDock.qml
        qml/components/VoiceRoom.qml
        qml/components/ParticipantTile.qml
        qml/components/Icon.qml
        qml/components/ThemedSwitch.qml
        qml/components/ThemedScrollBar.qml
        qml/components/ThemedComboBox.qml
        qml/components/ThemedCheckBox.qml
        qml/components/ThemedSlider.qml
        qml/components/InfoBanner.qml
        qml/components/MemberList.qml
        qml/components/LoginDialog.qml
        qml/components/UserProfileCard.qml
        qml/components/UserSettings.qml
        qml/components/ServerSettings.qml
        qml/components/ChannelSettings.qml
        qml/components/ClientSettings.qml
        qml/components/ForwardDialog.qml
        qml/components/ImageViewer.qml
        qml/components/ShortcutsDialog.qml
        qml/components/SearchPopup.qml
        qml/components/StatusPicker.qml
        qml/components/ThreadPanel.qml
        qml/components/RoleAssignPopup.qml
        qml/components/ToastHost.qml
        qml/components/VideoPlayerCard.qml
        qml/components/LinkPreview.qml
        qml/theme/Theme.qml
    RESOURCES
        qml/data/EmojiData.js
        qml/fonts/Geist-Variable.ttf
        qml/fonts/GeistMono-Variable.ttf
        qml/fonts/OFL.txt
        qml/icons/mic.svg
        qml/icons/mic-off.svg
        qml/icons/headphones.svg
        qml/icons/headphones-off.svg
        qml/icons/screen-share.svg
        qml/icons/video.svg
        qml/icons/phone-off.svg
        qml/icons/settings.svg
        qml/icons/hash.svg
        qml/icons/at.svg
        qml/icons/plus.svg
        qml/icons/x.svg
        qml/icons/chevron-down.svg
        qml/icons/chevron-right.svg
        qml/icons/send.svg
        qml/icons/smile.svg
        qml/icons/paperclip.svg
        qml/icons/search.svg
        qml/icons/bolt.svg
        qml/icons/shield.svg
        qml/icons/users.svg
        qml/icons/pin.svg
        qml/icons/signal.svg
        qml/icons/lock.svg
        qml/icons/reply.svg
        qml/icons/forward.svg
        qml/icons/edit.svg
        qml/icons/link.svg
        qml/icons/copy.svg
        qml/icons/play.svg
        qml/icons/pause.svg
        qml/icons/volume-off.svg
        qml/icons/smile-plus.svg
        qml/icons/more-horizontal.svg
        qml/icons/check.svg
        qml/icons/menu.svg
        qml/icons/annotate.svg
        qml/icons/crosshair.svg
        qml/icons/expand.svg
        qml/icons/eye.svg
        qml/icons/gamepad.svg
        qml/icons/gift.svg
        qml/icons/inbox.svg
        qml/icons/minus.svg
        qml/icons/screen.svg
        qml/icons/volume.svg
)

target_include_directories(bsfchat-app PRIVATE src)

target_link_libraries(bsfchat-app PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Sql Qt6::Multimedia Qt6::Svg Qt6::Widgets
)

if(BSFCHAT_ENABLE_VOICE)
    target_link_libraries(bsfchat-app PRIVATE datachannel-static opus)
endif()

# CoreGraphics gives us CGRequestScreenCaptureAccess to trigger the
# macOS TCC prompt for Screen Recording — Qt's QScreenCapture won't
# ask on its own.
if(APPLE AND NOT IOS)
    # Desktop-macOS only — ScreenCaptureKit is mac-exclusive, and
    # the others are mostly used by the capture paths that don't run
    # on iOS either. AVFoundation/CoreMedia DO exist on iOS but
    # we link them from the mobile-specific target separately when
    # that work lands.
    target_link_libraries(bsfchat-app PRIVATE
        "-framework CoreGraphics"
        "-framework ScreenCaptureKit"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation")

    # Stable dev-signing — if the user ran scripts/setup-dev-signing.sh
    # the "BSFChat Dev Signer" identity lives in the login keychain;
    # sign with it on every build so the cdhash stays stable across
    # rebuilds. Without this, macOS invalidates the Screen Recording
    # and Camera TCC grants on every recompile.
    execute_process(
        COMMAND security find-identity -v -p codesigning
        OUTPUT_VARIABLE _codesign_identities
        ERROR_QUIET)
    if(_codesign_identities MATCHES "BSFChat Dev Signer")
        message(STATUS "Using stable dev-signing identity 'BSFChat Dev Signer'")
        add_custom_command(TARGET bsfchat-app POST_BUILD
            COMMAND codesign --force --deep
                    --sign "BSFChat Dev Signer"
                    --preserve-metadata=entitlements
                    "$<TARGET_BUNDLE_DIR:bsfchat-app>"
            VERBATIM)
    else()
        message(STATUS "No stable dev-signing identity — builds will be adhoc-signed.")
        message(STATUS "  Run scripts/setup-dev-signing.sh once to fix TCC churn.")
    endif()
endif()

if(GAMECHAT_CLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
