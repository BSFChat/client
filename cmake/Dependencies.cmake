include(FetchContent)

# Protocol library (local path for development, GitHub for CI/Docker)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/CMakeLists.txt)
    FetchContent_Declare(bsfchat_protocol SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
else()
    # Pinned to main, matching server/ and identity/. This was stuck on
    # the video-rtp branch long after that work merged, which broke every
    # CI platform while local builds stayed green — the branch above uses
    # the sibling ../protocol checkout, so a stale remote pin is invisible
    # until CI runs. If this ever needs pinning again, pin to a TAG and
    # say when it goes back to main.
    FetchContent_Declare(bsfchat_protocol GIT_REPOSITORY https://github.com/BSFChat/protocol.git GIT_TAG main GIT_SHALLOW TRUE)
endif()

set(GAMECHAT_PROTOCOL_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(bsfchat_protocol)

# OS secure storage (macOS/iOS Keychain, Windows Credential Manager, Linux
# Secret Service via libsecret, Android Keystore) for the BSFChat ID refresh
# token, which can mint sign-ins to any server and so must never sit in the
# plain settings file. See src/core/SecretStore.h. OFF builds keep the token
# in memory only: signed in until the app quits, never on disk.
option(BSFCHAT_ENABLE_KEYCHAIN "Store sign-in secrets in the OS keychain" ON)
if(BSFCHAT_ENABLE_KEYCHAIN)
    FetchContent_Declare(
        qtkeychain
        GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
        GIT_TAG        0.15.0
        GIT_SHALLOW    TRUE
    )
    set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
    # Static, explicitly. QtKeychain declares option(BUILD_SHARED_LIBS ON),
    # and on a FRESH configure nothing has set BUILD_SHARED_LIBS yet (the
    # voice block below forces it OFF only afterwards), so CI built it
    # shared while a reconfigured local tree built it static. Shared broke
    # three platforms in v0.0.49-rc.1: Windows put qt6keychain.dll in
    # build/bin/ where no test (or windeployqt) finds it, and the loader's
    # "DLL not found" dialog hung ctest until the 6 h job timeout; the iOS
    # Xcode project made it a dylib target with no bundle identifier; and
    # no packaging step copies it next to the app. A plain (non-cache) OFF
    # is what option() honours under CMP0077 NEW, set just below.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
    set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
    # Its own autotests need a live keyring and are not ours to run. Its
    # include(CTest) declares option(BUILD_TESTING ON); a plain OFF here
    # (respected under CMP0077 NEW) keeps them out. Left set afterwards on
    # purpose: before QtKeychain nothing defined BUILD_TESTING, so the other
    # dependencies (opus, libaom) saw it false and registered no tests of
    # their own. Our tests/ do not depend on it.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTING OFF)
    FetchContent_MakeAvailable(qtkeychain)
    unset(CMAKE_POLICY_DEFAULT_CMP0077)
    unset(BUILD_SHARED_LIBS)
    # generate_export_header() leaves QKEYCHAIN_EXPORT as dllimport for
    # consumers unless this is defined; QtKeychain itself never sets it.
    target_compile_definitions(qt6keychain PUBLIC QT6KEYCHAIN_STATIC_DEFINE)
endif()

# Voice chat dependencies (libdatachannel + opus)
if(BSFCHAT_ENABLE_VOICE)
    # Codec/media deps link static. Something in the dependency graph
    # force-caches BUILD_SHARED_LIBS=ON, which turned aom/opus into
    # DLLs on Windows — windeployqt only gathers Qt DLLs, so every
    # non-Qt runtime DLL needs a hand-written packaging step, and
    # aom.dll shipped missing in v0.0.42's installer. Static removes
    # the entire class. (libdatachannel declares its shared target
    # explicitly, so datachannel.dll is unaffected and its existing
    # copy step still applies.)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        # 0.24.5 over 0.22.4: no CVE ids, but real network-facing fixes
        # after 0.22.4 — heap use-after-free in IceTransport::RecvCallback
        # (PR #1567), missing RTP/RTCP size checks (PR #1531), and a
        # bundled-libjuice STUN HMAC-key crash (>64B key).
        GIT_TAG        v0.24.5
        GIT_SHALLOW    TRUE
        # setRemoteDescription() hands ICE the remote credentials before it
        # stores the remote description, so on loopback the peer's DTLS
        # certificate can arrive while there is nothing to check its
        # fingerprint against — the handshake is then rejected outright
        # ("certificate verify failed" / "unknown ca"). Still unfixed on
        # upstream master. The script is idempotent and fails the configure
        # if its anchor ever stops matching, so moving the pin cannot
        # silently drop the fix. Full incident in the script's header.
        # The same script also makes VideoRtpDepacketizer drop a frame whose
        # reassembly throws and caps its buffer (one malformed STAP-A packet
        # otherwise stalls the track for good). v0.24.6 does not fix that.
        PATCH_COMMAND  ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/patch-libdatachannel.cmake"
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
    # Vendored libsrtp turns warnings-as-errors ON by default, under
    # different option names depending on the version the libdatachannel
    # pin carries. Third-party code must never fail our builds over
    # compiler-version warning drift — it broke all three v0.0.42
    # release builds on warnings local clang doesn't even emit.
    set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)        # libsrtp ≤2.5
    set(ENABLE_WARNINGS OFF CACHE BOOL "" FORCE)            # libsrtp ≥2.6
    set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)  # libsrtp ≥2.6
    # Its test apps build (and run flag probes) by default — dead weight.
    set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
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
        # The decoder takes frame dimensions from a peer's bitstream; AV1
        # allows 65536x65536. Refuse anything past 8192 inside libaom,
        # before it allocates (decoder-only; the encoder ignores these).
        # AomLosslessDecoder checks the same limit again.
        set(CONFIG_SIZE_LIMIT 1 CACHE STRING "" FORCE)
        set(DECODE_WIDTH_LIMIT 8192 CACHE STRING "" FORCE)
        set(DECODE_HEIGHT_LIMIT 8192 CACHE STRING "" FORCE)
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
