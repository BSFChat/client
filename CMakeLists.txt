cmake_minimum_required(VERSION 3.20...3.31)
project(bsfchat-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GAMECHAT_CLIENT_BUILD_TESTS "Build client tests" ON)
option(BSFCHAT_ENABLE_VOICE "Enable voice chat (requires libdatachannel + opus)" ON)

include(cmake/Dependencies.cmake)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Network Qml Quick QuickControls2 QuickDialogs2 Sql Multimedia Widgets)
qt_standard_project_setup()
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 OLD)

qt_add_executable(bsfchat-app
    src/main.cpp
    src/core/App.cpp
    src/core/NotificationManager.cpp
    src/core/Settings.cpp
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
)

if(BSFCHAT_ENABLE_VOICE)
    target_sources(bsfchat-app PRIVATE
        src/voice/VoiceEngine.cpp
        src/voice/PeerConnectionManager.cpp
        src/voice/AudioEngine.cpp
        src/voice/AudioMixer.cpp
        src/voice/SoundGenerator.cpp
        src/voice/NotificationSounds.cpp
    )
    target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_VOICE_ENABLED)
endif()

# Ensure macOS .app bundle is created
set_target_properties(bsfchat-app PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BSFChat"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.bsfchat.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.0.19"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.0.19"
    MACOSX_BUNDLE_INFO_STRING "BSFChat"
    WIN32_EXECUTABLE TRUE
)

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
                -string "BSFChat does not use the camera."
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
    QML_FILES
        qml/main.qml
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
    Qt6::Core Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Sql Qt6::Multimedia Qt6::Widgets
)

if(BSFCHAT_ENABLE_VOICE)
    target_link_libraries(bsfchat-app PRIVATE datachannel-static opus)
endif()

if(GAMECHAT_CLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
