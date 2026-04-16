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
find_package(Qt6 REQUIRED COMPONENTS Core Network Qml Quick QuickControls2 QuickDialogs2 Sql Multimedia)
qt_standard_project_setup()
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 OLD)

qt_add_executable(bsfchat-app
    src/main.cpp
    src/core/App.cpp
    src/core/Settings.cpp
    src/identity/IdentityClient.cpp
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
    MACOSX_BUNDLE_BUNDLE_VERSION "0.0.13"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.0.13"
    MACOSX_BUNDLE_INFO_STRING "BSFChat - Bullshit Free Chat"
    WIN32_EXECUTABLE TRUE
)

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
        qml/components/VoicePanel.qml
        qml/components/MemberList.qml
        qml/components/LoginDialog.qml
        qml/components/UserProfileCard.qml
        qml/components/UserSettings.qml
        qml/components/ServerSettings.qml
        qml/components/ChannelSettings.qml
        qml/theme/Theme.qml
    RESOURCES
        qml/data/EmojiData.js
)

target_include_directories(bsfchat-app PRIVATE src)

target_link_libraries(bsfchat-app PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Sql Qt6::Multimedia
)

if(BSFCHAT_ENABLE_VOICE)
    target_link_libraries(bsfchat-app PRIVATE datachannel-static opus)
endif()

if(GAMECHAT_CLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
