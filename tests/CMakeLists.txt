find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_models
    test_models.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ServerListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)

target_include_directories(test_models PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_models PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)

add_test(NAME test_models COMMAND test_models)

# Startup-cost path: the persisted sync token + room snapshot that lets a
# launch skip the server's initial /sync, and the retry schedule that keeps a
# downed server from being dialled every 5s forever. Both are deliberately
# free of the event loop and the network, so this needs no server.
qt_add_executable(test_sync_resume
    test_sync_resume.cpp
    ${CMAKE_SOURCE_DIR}/src/store/LocalCache.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncBackoff.cpp
)
target_include_directories(test_sync_resume PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_sync_resume PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Sql Qt6::Test
)
add_test(NAME test_sync_resume COMMAND test_sync_resume)

# Codec round-trip tests — only meaningful where a software H.264
# backend exists (openh264: macOS/Linux desktop).
if(BSFCHAT_ENABLE_VOICE AND BSFCHAT_HAVE_OPENH264)
    qt_add_executable(test_video_codec
        test_video_codec.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoCodecFactory.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/FrameConverter.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Decoder.cpp
    )
    target_include_directories(test_video_codec PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${libyuv_SOURCE_DIR}/include)
    target_compile_definitions(test_video_codec PRIVATE BSFCHAT_HAVE_OPENH264)
    target_link_libraries(test_video_codec PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test
        yuv openh264::openh264
    )
    # Include the VideoToolbox backend on macOS so the round-trip and
    # cross-backend (VT ↔ openh264, i.e. mac ↔ linux) tests cover the
    # hardware path.
    if(APPLE AND NOT IOS)
        target_sources(test_video_codec PRIVATE
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTEncoder.mm
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTDecoder.mm
        )
        target_compile_definitions(test_video_codec PRIVATE BSFCHAT_HAVE_VIDEOTOOLBOX)
        target_link_libraries(test_video_codec PRIVATE
            "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
    endif()
    if(BSFCHAT_HAVE_AOM)
        target_sources(test_video_codec PRIVATE
            ${CMAKE_SOURCE_DIR}/src/voice/video/AomLosslessEncoder.cpp
            ${CMAKE_SOURCE_DIR}/src/voice/video/AomLosslessDecoder.cpp
        )
        target_compile_definitions(test_video_codec PRIVATE BSFCHAT_HAVE_AOM)
        target_link_libraries(test_video_codec PRIVATE aom)
        # Same old-style-CMake story as bsfchat-app: the aom target
        # exports no usage requirements, so consumers add its source
        # root (public headers) and build dir (generated config) by
        # hand. Missing here = "aom/aom_encoder.h not found" on CI.
        target_include_directories(test_video_codec PRIVATE
            ${libaom_SOURCE_DIR} ${libaom_BINARY_DIR})
    endif()
    add_test(NAME test_video_codec COMMAND test_video_codec)
endif()

# Voice receive jitter buffer: reordering, PLC on gaps, late/duplicate
# discard and uint16 sequence wraparound. Pure logic — no Qt Multimedia,
# no audio device, no network. Links opus for the decoder handle.
if(BSFCHAT_ENABLE_VOICE)
    qt_add_executable(test_jitter_buffer
        test_jitter_buffer.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/JitterBuffer.cpp
    )
    target_include_directories(test_jitter_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_jitter_buffer PRIVATE Qt6::Core Qt6::Test opus)
    add_test(NAME test_jitter_buffer COMMAND test_jitter_buffer)
endif()

# Thread handoff between the network side and the audio thread: FIFO
# order per producer, no loss below capacity, RemovePeer ordering, and
# the bounded-drop behaviour — asserted under real contention from four
# producer threads. Also drives a JitterBuffer from the consumer side to
# cover the "single-threaded jitter buffer, packets arrive from
# elsewhere" invariant. No audio device, no Qt Multimedia, no network.
if(BSFCHAT_ENABLE_VOICE)
    qt_add_executable(test_audio_handoff
        test_audio_handoff.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/AudioPacketQueue.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/JitterBuffer.cpp
    )
    target_include_directories(test_audio_handoff PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_audio_handoff PRIVATE Qt6::Core Qt6::Test opus)
    add_test(NAME test_audio_handoff COMMAND test_audio_handoff)
    set_tests_properties(test_audio_handoff PROPERTIES TIMEOUT 120)
endif()

# Mesh-vs-SFU transport selection. Pure logic — no transport instances,
# no libdatachannel, no LiveKit SDK, no audio device, no network — so it
# runs everywhere including builds configured without BSFCHAT_ENABLE_VOICE.
qt_add_executable(test_voice_transport
    test_voice_transport.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceTransportSelector.cpp
)
target_include_directories(test_voice_transport PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_voice_transport PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_voice_transport COMMAND test_voice_transport)

# Media-key parsing and the security labelling. Pure logic — no SDK, no
# network. The labelling tests assert the ABSENCE of an end-to-end claim,
# which is the specific mistake this project has already made once.
qt_add_executable(test_voice_encryption
    test_voice_encryption.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceEncryption.cpp
)
target_include_directories(test_voice_encryption PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The labelling tests also scan the QML tree for hard-coded security
# strings — the mistake that put "DTLS · SCTP" in VoiceRoom.qml with no
# C++ behind it. Source path rather than an installed copy: the point is
# to fail on what someone just wrote, before it is built into anything.
target_compile_definitions(test_voice_encryption PRIVATE
    BSFCHAT_QML_DIR="${CMAKE_SOURCE_DIR}/qml")
target_link_libraries(test_voice_encryption PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_voice_encryption COMMAND test_voice_encryption)

# LiveKit vendoring gate. Not a transport test — there is no transport
# yet. It proves the vendored SDK compiles against our flags, links, and
# (the part that matters) that BOTH shared libraries are loadable from
# where the bundling step put them. liblivekit resolves liblivekit_ffi
# lazily at load time, so a missing ffi library otherwise links, packages
# and ships silently, exactly like aom.dll did in v0.0.42. Because this
# executable calls an exported symbol, it cannot even start unless both
# libraries are present — so that failure becomes a red test.
if(BSFCHAT_HAVE_LIVEKIT)
    qt_add_executable(test_livekit_sdk
        test_livekit_sdk.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/LiveKitSdkProbe.cpp
    )
    target_include_directories(test_livekit_sdk PRIVATE ${CMAKE_SOURCE_DIR}/src)
    # Runtime library basenames, so the test asserts co-location against
    # the same names cmake/LiveKit.cmake copies rather than a second
    # hardcoded per-platform list that could drift.
    cmake_path(GET BSFCHAT_LIVEKIT_RUNTIME FILENAME _lk_rt_name)
    cmake_path(GET BSFCHAT_LIVEKIT_FFI_RUNTIME FILENAME _lk_ffi_rt_name)
    target_compile_definitions(test_livekit_sdk PRIVATE
        BSFCHAT_HAVE_LIVEKIT
        BSFCHAT_LIVEKIT_PINNED_VERSION="${BSFCHAT_LIVEKIT_VERSION}"
        # Two macros, not one semicolon-separated string: CMake treats
        # ';' as a list separator inside target_compile_definitions and
        # would split it into two malformed -D flags.
        BSFCHAT_LIVEKIT_RUNTIME_NAME="${_lk_rt_name}"
        BSFCHAT_LIVEKIT_FFI_RUNTIME_NAME="${_lk_ffi_rt_name}")
    target_link_libraries(test_livekit_sdk PRIVATE
        Qt6::Core Qt6::Test livekit::livekit)
    bsfchat_livekit_bundle_runtime(test_livekit_sdk)
    add_test(NAME test_livekit_sdk COMMAND test_livekit_sdk)

    # PlatformAudio device-enumeration prototype. Built but NOT registered
    # as a ctest: it constructs WebRTC's Audio Device Module and reports what
    # the host actually has, so its output is machine-specific and it is a
    # diagnostic to run by hand, not a pass/fail assertion. It never calls
    # createAudioSource(), so it does not start capture or trigger a
    # microphone permission prompt.
    qt_add_executable(platform_audio_probe
        platform_audio_probe_main.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/PlatformAudioProbe.cpp
    )
    target_include_directories(platform_audio_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(platform_audio_probe PRIVATE
        Qt6::Core Qt6::Multimedia livekit::livekit)
    bsfchat_livekit_bundle_runtime(platform_audio_probe)
endif()

# Update channel selection + semver precedence. Header-only logic
# (src/core/ReleaseSelection.h), so there is nothing to compile but the
# test itself: no QNetworkAccessManager, no event loop, no GitHub, and
# no dependency on BSFCHAT_ENABLE_VOICE — it runs in every configuration.
qt_add_executable(test_update_channel test_update_channel.cpp)
target_include_directories(test_update_channel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_update_channel PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_update_channel COMMAND test_update_channel)

# Media-path loopback: proves H.264 AUs survive packetize → DTLS-SRTP →
# depacketize between two in-process peer connections using the exact
# handler chain production builds. No Qt, no codecs, no network.
if(BSFCHAT_ENABLE_VOICE)
    add_executable(test_media_loopback test_media_loopback.cpp)
    target_link_libraries(test_media_loopback PRIVATE LibDataChannel::LibDataChannel)
    # Not registered on Windows: GitHub's Windows runners stall the
    # loopback's UDP host-candidate binds behind the Defender firewall
    # and the run times out (build still compiles the test). Run it
    # manually on real Windows machines.
    if(NOT WIN32)
        add_test(NAME test_media_loopback COMMAND test_media_loopback)
        set_tests_properties(test_media_loopback PROPERTIES TIMEOUT 60)
    endif()
endif()
