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

# Native gamepad input (SDL3) — src/input/.
#
# Qt 6 deleted QtGamepad and shipped nothing in its place, so the choice
# was SDL or three hand-written platform backends (evdev, XInput,
# IOKit/GameController). SDL's *gamepad* API — as opposed to its joystick
# API — carries a ~900-entry mapping database compiled into the library,
# which is what makes "the south face button" and "the left trigger" mean
# the same thing on an Xbox pad, a DualSense, a Switch Pro controller and
# a Steam Deck with no per-device code on our side. There is no data file
# to ship or keep current. See src/input/GamepadBridge.h.
#
# DESKTOP ONLY, and gated OFF for both mobile targets rather than fought:
#
#   * Android. SDL's Android backend is not a library you link, it is an
#     Activity: it expects SDLActivity (its own Java class) to own the
#     process's lifecycle, surface and input dispatch. This app's Android
#     entry point is Qt's QtActivity, via our own patched manifest and
#     package source dir (see cmake/AndroidJdk.cmake and android/). Two
#     frameworks cannot both own the Activity. Even if they could, the
#     feature is pointless there: nobody plays with a pad in one hand and
#     a phone running a chat client in the other, and the Deck — the
#     device this is actually for — runs the desktop Linux build.
#
#   * iOS. SDL's MFi backend would technically work, but it would add a
#     static library, GameController framework linkage and an
#     unexplained-input-device line to every App Store review, for a
#     feature with no iOS use case. The mobile builds are already the
#     tightest constraint this project has (see docs/ios-release.md);
#     spending any of that budget here would be a poor trade.
#
# With this OFF, src/input still compiles and GamepadBridge still exists
# and is still registered — it simply reports no controller, exactly as
# core/Haptics no-ops on desktop. That is deliberate: it keeps the
# platform gate out of main.cpp and out of QML.
if(ANDROID OR IOS)
    set(BSFCHAT_ENABLE_GAMEPAD OFF)
else()
    option(BSFCHAT_ENABLE_GAMEPAD
        "Native gamepad input via SDL3 (desktop only)" ON)
endif()

if(BSFCHAT_ENABLE_GAMEPAD)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        # 3.4.x is a stable line (SDL uses even minors for stable, odd for
        # development). Pinned to a tag, not a branch: a mapping-database
        # change is exactly the kind of thing that would alter which
        # physical button our South means, and that must never move
        # without someone choosing to move it.
        GIT_TAG        release-3.4.18
        GIT_SHALLOW    TRUE
    )

    # Static, for the same reason the codec deps are (see the comment at
    # the top of the voice block): windeployqt gathers only Qt DLLs, so
    # every non-Qt runtime DLL needs a hand-written packaging step, and
    # the one time that was missed it shipped a broken installer.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL_CPACK OFF CACHE BOOL "" FORCE)
    set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)

    # ONLY the gamepad path. This is the load-bearing part of this block.
    #
    # This application already owns its audio devices (src/voice/), its
    # rendering (Qt Quick) and its event loop. A second library that also
    # believes it owns any of those is not a larger dependency, it is a
    # conflict: SDL's audio backend opens the same CoreAudio / WASAPI /
    # PulseAudio devices AudioWorker is holding, and SDL_INIT_VIDEO
    # installs platform event hooks that compete with Qt's. Compiling
    # those subsystems out means the conflict is not merely unused at
    # runtime, it is absent from the binary — so a later SDL_Init flag
    # typo cannot resurrect it.
    #
    # JOYSTICK is the subsystem the gamepad API is built on; there is no
    # separate SDL_GAMEPAD option. HIDAPI stays on because it is what
    # gives DualSense / Switch Pro / 8BitDo pads their correct mappings
    # and full button sets rather than a generic fallback.
    #
    # Not needed, and each one off for its own reason:
    #   AUDIO/VIDEO/RENDER/GPU/CAMERA — owned by this app, see above.
    #   HAPTIC  — the rumble we might want later is SDL_RumbleGamepad,
    #             which is joystick-side and needs no haptic subsystem.
    #   POWER   — system battery. Controller battery is SDL_GetGamepadPowerInfo,
    #             also joystick-side.
    #   SENSOR  — DualSense gyro/accelerometer. No use case here.
    #   DIALOG  — native file pickers. Qt's are already used.
    #   TRAY    — tray icon. QSystemTrayIcon already does this.
    set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_TRAY OFF CACHE BOOL "" FORCE)

    # Linux desktop integration bits that only exist to serve a window.
    # With SDL_VIDEO off there is no window, and leaving these on would
    # add libdbus / libibus / libfcitx header requirements to the Linux
    # builder for code that cannot run.
    set(SDL_DBUS OFF CACHE BOOL "" FORCE)
    set(SDL_IBUS OFF CACHE BOOL "" FORCE)

    # libudev stays ON (the default) and is the one Linux system
    # dependency this adds. It is how SDL learns about a controller being
    # plugged in; without it SDL falls back to rescanning /dev/input on a
    # timer, which still works but makes hotplug sluggish. SDL dlopen()s
    # libudev at runtime (SDL_UDEV_DYNAMIC), so this is a HEADER-only
    # build requirement — hence libudev-dev in the Linux CI apt list and
    # no new runtime .so in the tarball.
    set(SDL_LIBUDEV ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(SDL3)
endif()
