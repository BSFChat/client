include(FetchContent)

# Protocol library (local path for development, GitHub for CI/Docker)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/CMakeLists.txt)
    FetchContent_Declare(bsfchat_protocol SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
else()
    # NOTE: pinned to the video-rtp branch (bsfchat.call.negotiate
    # constant) while the RTP video migration is in review — flip back
    # to main when both branches merge.
    FetchContent_Declare(bsfchat_protocol GIT_REPOSITORY https://github.com/BSFChat/protocol.git GIT_TAG video-rtp GIT_SHALLOW TRUE)
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

    # libaom — AV1 reference codec (BSD-2), used exclusively for the
    # mathematically-lossless tier (AV1E_SET_LOSSLESS + identity-matrix
    # I444). Desktop only; needs nasm on x86 hosts.
    if(NOT ANDROID AND NOT IOS)
        FetchContent_Declare(
            libaom
            GIT_REPOSITORY https://aomedia.googlesource.com/aom
            GIT_TAG        v3.12.1
        )
        set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
        set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)
        # libaom's optional internal libyuv OBJECT target is also named
        # `yuv` and collides with the real libyuv above; it's only used
        # by examples/tuning we don't build.
        set(CONFIG_LIBYUV 0 CACHE INTERNAL "" FORCE)
        FetchContent_MakeAvailable(libaom)
        set(BSFCHAT_HAVE_AOM ON)
    endif()

    # openh264 — BSD H.264 software encoder/decoder. The Linux default
    # codec and the software fallback on macOS. NOT used on Windows
    # (Media Foundation ships a guaranteed software H.264 MFT) or on
    # mobile. Vendored prebuilt (make-based upstream build, so no
    # FetchContent) — produce it with scripts/build-openh264.sh, which
    # drops a static lib + headers into deps/openh264-<platform>/.
    if(NOT WIN32 AND NOT ANDROID AND NOT IOS)
        if(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
                set(_oh264_dir ${CMAKE_CURRENT_SOURCE_DIR}/deps/openh264-macos-arm64)
            else()
                set(_oh264_dir ${CMAKE_CURRENT_SOURCE_DIR}/deps/openh264-macos-x64)
            endif()
        else()
            set(_oh264_dir ${CMAKE_CURRENT_SOURCE_DIR}/deps/openh264-linux-x64)
        endif()
        if(NOT EXISTS ${_oh264_dir}/lib/libopenh264.a)
            message(FATAL_ERROR
                "Vendored openh264 not found at ${_oh264_dir}. "
                "Run scripts/build-openh264.sh first.")
        endif()
        add_library(openh264::openh264 STATIC IMPORTED)
        set_target_properties(openh264::openh264 PROPERTIES
            IMPORTED_LOCATION ${_oh264_dir}/lib/libopenh264.a
            INTERFACE_INCLUDE_DIRECTORIES ${_oh264_dir}/include)
        set(BSFCHAT_HAVE_OPENH264 ON)
    endif()

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
