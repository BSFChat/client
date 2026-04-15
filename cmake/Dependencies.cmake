include(FetchContent)

# Protocol library (local path for development, GitHub for CI/Docker)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/CMakeLists.txt)
    FetchContent_Declare(bsfchat_protocol SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
else()
    FetchContent_Declare(bsfchat_protocol GIT_REPOSITORY https://github.com/BSFChat/protocol.git GIT_TAG main GIT_SHALLOW TRUE)
endif()

set(GAMECHAT_PROTOCOL_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(bsfchat_protocol)

# Voice chat dependencies (libdatachannel + opus)
if(BSFCHAT_ENABLE_VOICE)
    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.21.2
        GIT_SHALLOW    TRUE
    )
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libdatachannel)

    FetchContent_Declare(
        opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG        v1.5.2
        GIT_SHALLOW    TRUE
    )
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(opus)
endif()
