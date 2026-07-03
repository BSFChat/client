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
        # 0.24.5 over 0.22.4: no CVE ids, but real network-facing fixes
        # after 0.22.4 — heap use-after-free in IceTransport::RecvCallback
        # (PR #1567), missing RTP/RTCP size checks (PR #1531), and a
        # bundled-libjuice STUN HMAC-key crash (>64B key).
        GIT_TAG        v0.24.5
        GIT_SHALLOW    TRUE
    )
    # Media transport ON: rtc::Track + RTP packetizers + DTLS-SRTP for
    # the real-video path (H.264 over RTP). SRTP comes from the libsrtp
    # checkout vendored inside libdatachannel (deps/libsrtp) — built
    # statically with the OpenSSL backend the build already locates, so
    # no new runtime libraries appear in the bundles.
    set(NO_MEDIA OFF CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libdatachannel)

    # libyuv — pixel-format conversion + scaling for the video encode/
    # decode pipeline (QVideoFrame BGRA ↔ I420/NV12, and the identity-
    # matrix I444 path the lossless tier needs). BSD, CMake, static.
    # Pinned to the chromium `stable` branch head as of 2026-07.
    FetchContent_Declare(
        libyuv
        GIT_REPOSITORY https://chromium.googlesource.com/libyuv/libyuv
        GIT_TAG        eb6e7bb63738e29efd82ea3cf2a115238a89fa51
    )
    # Keep libyuv's optional MJPEG module off: it enables itself when a
    # system libjpeg is *found* but never links it, breaking the shared
    # lib / tools targets. We only use the conversion/scale kernels.
    set(CMAKE_DISABLE_FIND_PACKAGE_JPEG TRUE)
    FetchContent_MakeAvailable(libyuv)
    unset(CMAKE_DISABLE_FIND_PACKAGE_JPEG)
    # Only the static `yuv` target is linked; keep the shared lib and
    # conversion tool out of the default build.
    foreach(_yuv_extra yuv_shared yuvconvert i444tonv12_eg)
        if(TARGET ${_yuv_extra})
            set_target_properties(${_yuv_extra} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()

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
