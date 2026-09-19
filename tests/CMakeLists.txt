find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_models
    test_models.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ThreadFilterModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ServerListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)

target_include_directories(test_models PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The /members rebuild guard reads MemberListModel.cpp and net/MemberJson.h to
# check that every content key the model reads is named in the rebuild.
target_compile_definitions(test_models PRIVATE
    BSFCHAT_SRC_DIR="${CMAKE_SOURCE_DIR}/src")

target_link_libraries(test_models PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)

add_test(NAME test_models COMMAND test_models)

# --profile <name> namespace resolution (multi-instance support). Pure
# logic: no GUI app is constructed, because launching bsfchat-app on a dev
# Mac sets off macOS permission dialogs.
qt_add_executable(test_profile
    test_profile.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
)
target_include_directories(test_profile PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_profile PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_profile COMMAND test_profile)

# Profile/nickname reply parsing. The regression it guards is a server
# answering a bare `null` for an account with no display name and no picture,
# which threw out of nlohmann's value() and into an empty catch — no signal, no
# log, callers waiting forever. Header-only, so no server and no GUI app.
qt_add_executable(test_reply_fields
    test_reply_fields.cpp
)
target_include_directories(test_reply_fields PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_reply_fields PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)
add_test(NAME test_reply_fields COMMAND test_reply_fields)

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

# Voice join/leave lifecycle — the VoiceSession state machine plus the two
# pure helpers the join path leans on (roster reconciliation, start
# refusal) and the transport selector call joinVoiceChannel now makes.
#
# This is the seam Phase 0 of the September plan calls for: no server, no
# libdatachannel, no audio device, no GUI. VoiceSession reaches the network
# through signals and the transport through std::function hooks, so the
# test substitutes a recording fake for both. Before it, the entire join
# path had zero coverage.
qt_add_executable(test_voice_lifecycle
    test_voice_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/net/VoiceSession.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceTransportSelector.cpp
)
target_include_directories(test_voice_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_voice_lifecycle PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_voice_lifecycle COMMAND test_voice_lifecycle)

# The m.call.* wire format, both ends at once (V-C3): the builders
# VoiceEngine sends with, taken through the protocol's real sync parser,
# read back by the parser ServerConnection dispatches with. The failure
# this exists for — an answer arriving with an empty call id, silently
# discarded forever — was invisible precisely because the two ends were
# separate literal key sets in separate files.
qt_add_executable(test_call_signalling
    test_call_signalling.cpp
)
target_include_directories(test_call_signalling PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_call_signalling PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)
add_test(NAME test_call_signalling COMMAND test_call_signalling)

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

# Video pipeline logic with no other coverage: the RTP-vs-JPEG routing
# predicates (S-1), the receive side's keyframe re-request loop (S-2),
# the RTP reorder window (S-4), the delivery-ratio estimate that feeds
# the rate controller and the control law itself (S-3), and the send
# pipeline's session rebuild / forced IDR (S-11, S-16). Needs a software
# H.264 backend for the send-pipeline cases, same condition as
# test_video_codec; the pure-logic cases would run anywhere.
if(BSFCHAT_ENABLE_VOICE AND BSFCHAT_HAVE_OPENH264)
    qt_add_executable(test_video_pipeline
        test_video_pipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoCodecFactory.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/FrameConverter.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/OpenH264Decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoRateController.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoReceivePipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoSendPipeline.cpp
        # The "this machine cannot decode what it advertised" latch, and
        # AppProfile because it persists its hint through a QSettings
        # keyed on the profile-aware app name — which is also how the
        # test isolates itself from the real install's settings file.
        ${CMAKE_SOURCE_DIR}/src/voice/video/VideoDecodeHealth.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
    )
    # PeerCaps.h parses the caps JSON, so the routing-predicate tests
    # need the same header-only json the app builds against.
    target_include_directories(test_video_pipeline PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${libyuv_SOURCE_DIR}/include
        ${nlohmann_json_SOURCE_DIR}/include)
    target_compile_definitions(test_video_pipeline PRIVATE BSFCHAT_HAVE_OPENH264)
    target_link_libraries(test_video_pipeline PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test
        yuv openh264::openh264)
    if(APPLE AND NOT IOS)
        target_sources(test_video_pipeline PRIVATE
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTEncoder.mm
            ${CMAKE_SOURCE_DIR}/src/voice/video/MacVTDecoder.mm
        )
        target_compile_definitions(test_video_pipeline PRIVATE
            BSFCHAT_HAVE_VIDEOTOOLBOX)
        target_link_libraries(test_video_pipeline PRIVATE
            "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
    endif()
    if(BSFCHAT_HAVE_AOM)
        target_sources(test_video_pipeline PRIVATE
            ${CMAKE_SOURCE_DIR}/src/voice/video/AomLosslessEncoder.cpp
            ${CMAKE_SOURCE_DIR}/src/voice/video/AomLosslessDecoder.cpp
        )
        target_compile_definitions(test_video_pipeline PRIVATE BSFCHAT_HAVE_AOM)
        target_link_libraries(test_video_pipeline PRIVATE aom)
        target_include_directories(test_video_pipeline PRIVATE
            ${libaom_SOURCE_DIR} ${libaom_BINARY_DIR})
    endif()
    add_test(NAME test_video_pipeline COMMAND test_video_pipeline)
    # Encodes real frames against QTRY deadlines; flaky under parallel ctest.
    set_tests_properties(test_video_pipeline PROPERTIES RUN_SERIAL TRUE)
    # Headless: no window, no capture device, no audio.
    set_tests_properties(test_video_pipeline PROPERTIES
        TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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

# Live audio device selection: which device the voice pipeline should be
# on, given the saved preference, the current device set and what is
# already open — plus the debounce that collapses a Bluetooth connect's
# notification burst into one restart.
#
# The bug this covers is that the answer used to be computed once, at
# join, so AirPods that connected mid-call were never used even after
# macOS made them the system default. Re-deciding on every change means
# the decision has to be pure, which is also the only way to test
# "switches back when the headset reappears" on a box with no sound card.
# Header-only, no Qt Multimedia, no audio thread — so it runs everywhere,
# including builds configured without BSFCHAT_ENABLE_VOICE.
qt_add_executable(test_audio_device_policy test_audio_device_policy.cpp)
target_include_directories(test_audio_device_policy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audio_device_policy PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_audio_device_policy COMMAND test_audio_device_policy)

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

# "Hide my IP address": the ICE transport-policy decision table. Pure logic —
# no libdatachannel, no TURN server, no network, no audio device — so the whole
# 2x2x2x2 of (user setting, per-share option, server allow_p2p, TURN present)
# is asserted without any of them, including the rows whose wrong answer is a
# silently-published address.
qt_add_executable(test_ip_privacy
    test_ip_privacy.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/IpPrivacy.cpp
)
target_include_directories(test_ip_privacy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_ip_privacy PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_ip_privacy COMMAND test_ip_privacy)

# The relay path against a REAL coturn, in a Docker container.
#
# The only check in this tree that can prove the privacy claim rather than the
# decision behind it: that rtc::TransportPolicy::Relay actually stops this
# machine's LAN and public addresses reaching the ICE exchange. There is no
# gatherer in a unit test, so there is nothing else that can tell a correct
# implementation from one that reports success and sends the address anyway.
#
# The TARGET always builds where voice is enabled — a test that stops compiling
# is a test that is gone — but it is only REGISTERED with ctest under
# -DBSFCHAT_RELAY_E2E=ON. It needs Docker, binds UDP ports on the host, and
# takes a few seconds, none of which belongs in the sweep run before a commit.
if(BSFCHAT_ENABLE_VOICE)
    option(BSFCHAT_RELAY_E2E
           "Register the coturn-backed relay end-to-end test with ctest" OFF)
    qt_add_executable(test_relay_e2e test_relay_e2e.cpp)
    target_include_directories(test_relay_e2e PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_relay_e2e PRIVATE
        Qt6::Core Qt6::Network Qt6::Test datachannel-static)
    if(BSFCHAT_RELAY_E2E)
        # The script starts coturn, exports the credentials, runs the binary and
        # tears the container down. The binary's path is a generator expression
        # rather than a guessed one, so it always exercises THIS build tree.
        add_test(NAME relay_e2e
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/e2e/relay_e2e.sh)
        set_tests_properties(relay_e2e PROPERTIES
            LABELS e2e
            ENVIRONMENT "BSFCHAT_RELAY_TEST_BIN=$<TARGET_FILE:test_relay_e2e>"
            TIMEOUT 180
            # Binds fixed UDP ports and a named container; two at once collide.
            RUN_SERIAL TRUE)
    endif()
endif()

# Media-key parsing and the security labelling. Pure logic — no SDK, no
# network. The labelling tests assert the ABSENCE of an end-to-end claim,
# which is the specific mistake this project has already made once.
qt_add_executable(test_voice_encryption
    test_voice_encryption.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceEncryption.cpp
    # The same scan now covers the second thing this client can overclaim —
    # whether an IP address is hidden. Same rule, same home for the wording.
    ${CMAKE_SOURCE_DIR}/src/voice/IpPrivacy.cpp
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
        # Real sockets, real DTLS handshakes and wall-clock deadlines: under
        # `ctest -j4` on a loaded CI runner this timed out twice in one day
        # (rc.11). Serial, so it never competes with the video pipeline test.
        set_tests_properties(test_media_loopback PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    endif()
endif()

# Teardown safety for PeerConnectionManager (V-C2 / S-6): two real
# managers over an in-process loopback, destroyed while audio, JPEG and
# RTP video are in flight. Meant to be run under ASan+UBSan, where a late
# libdatachannel callback into the freed object is a hard failure rather
# than a coin flip — see the header of the test for the build line.
# Registered as a plain ctest too, where it still catches a teardown that
# crashes or hangs outright.
if(BSFCHAT_ENABLE_VOICE)
    qt_add_executable(test_voice_teardown
        test_voice_teardown.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/PeerConnectionManager.cpp
    )
    target_include_directories(test_voice_teardown PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_voice_teardown PRIVATE
        bsfchat_protocol
        Qt6::Core Qt6::Test LibDataChannel::LibDataChannel)
    # Same reason test_media_loopback is not registered on Windows: the
    # runners stall loopback UDP host-candidate binds behind Defender.
    if(NOT WIN32)
        add_test(NAME test_voice_teardown COMMAND test_voice_teardown)
        set_tests_properties(test_voice_teardown PROPERTIES TIMEOUT 180)
    endif()
endif()

# RTP video RECEPTION between two real PeerConnectionManagers (S-16).
# test_media_loopback proves the handler chain packetizes and
# depacketizes; it uses a single m-line, which silently skips
# libdatachannel's SSRC demultiplexer. This one runs the production
# two-m-line setup and asserts that videoFrameReceived actually fires on
# the far side, in both directions, on both streams.
if(BSFCHAT_ENABLE_VOICE)
    qt_add_executable(test_video_rtp_loopback
        test_video_rtp_loopback.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/PeerConnectionManager.cpp
    )
    target_include_directories(test_video_rtp_loopback PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_video_rtp_loopback PRIVATE
        bsfchat_protocol
        Qt6::Core Qt6::Test LibDataChannel::LibDataChannel)
    # Not on Windows, for the same reason as the other two loopback
    # tests: the runners stall UDP host-candidate binds behind Defender.
    if(NOT WIN32)
        add_test(NAME test_video_rtp_loopback COMMAND test_video_rtp_loopback)
        # Real DTLS handshakes plus four wall-clock delivery windows.
        set_tests_properties(test_video_rtp_loopback PROPERTIES
            TIMEOUT 300 RUN_SERIAL TRUE)
    endif()
endif()

# Video-player playback maths (qml/js/PlaybackMath.js), driven through a
# QML test runner because that is where the code lives and where the app
# calls it — a C++ copy of the same arithmetic would prove nothing about
# the file VideoPlayerCard.qml actually imports.
#
# Scope is deliberately narrow: time formatting, seek clamping, the
# click-to-seek pixel→ms mapping, and the leave-fullscreen visibility
# rule (tst_playbackmath.qml), plus the Qt input behaviour the control
# bar depends on, checked against a replica of that bar because the real
# component imports the BSFChat module and only the app binary has it
# (tst_transportbarinput.qml — read its header before trusting it).
# Everything else in the player is hit-testing against a live window.
#
# The same runner picks up tests/qml/tst_videostage.qml, which covers the
# voice room's video stage (qml/js/VideoStage.js): grid shape, which feed
# is on the big panel, and where every thumbnail lands. Same reason it
# lives in a .js file — VoiceRoom.qml imports the BSFChat module, which
# only the app binary has.
#
# And tests/qml/tst_channelselection.qml, over qml/js/ChannelSelection.js:
# which channel-sidebar row is highlighted once the voice CONNECTION and
# the displayed VIEW disagree. ChannelList.qml imports the BSFChat module
# too, so the predicate moved out to a .js file to be reachable at all.
find_package(Qt6 QUIET COMPONENTS QuickTest)
if(Qt6QuickTest_FOUND AND NOT ANDROID AND NOT IOS)
    qt_add_executable(test_playback_math qml_test_main.cpp)
    target_compile_definitions(test_playback_math PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(test_playback_math PRIVATE
        Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickTest)
    add_test(NAME test_playback_math COMMAND test_playback_math)
    # Headless: this asserts on arithmetic, so it must not need a display
    # (or steal focus from whoever is at the keyboard when ctest runs).
    set_tests_properties(test_playback_math PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# ServerManager's connection-list lifecycle (U-C1). Header-only logic
# (src/net/ServerRoster.h) lifted out of ServerManager precisely so this
# needs no Settings, no ServerConnection, no event loop and no network.
qt_add_executable(test_server_roster test_server_roster.cpp)
target_include_directories(test_server_roster PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_server_roster PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_server_roster COMMAND test_server_roster)

# "Manage Account" after a restart: the per-server identity provider was
# persisted and read by nobody, so a self-hosted deployment's users were
# sent to the hosted portal on every launch after the first. Links the
# real ServerConnection (voice compiled out here, as everywhere outside
# bsfchat-app) because the fallback lives in its accessor; the object
# touches the network only once given credentials, which this never does.
qt_add_executable(test_identity_provider_url
    test_identity_provider_url.cpp
    ${CMAKE_SOURCE_DIR}/src/net/ServerConnection.cpp
    # ServerConnection gained a BotAdminModel* Q_PROPERTY when feat/bots merged.
    # AUTOMOC instantiates the pointer meta-type, which needs the complete type,
    # so this target has to carry the model even though the test never uses it.
    ${CMAKE_SOURCE_DIR}/src/model/BotAdminModel.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MatrixClient.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncLoop.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncBackoff.cpp
    ${CMAKE_SOURCE_DIR}/src/net/VoiceSession.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
    ${CMAKE_SOURCE_DIR}/src/identity/IdentityClient.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AudioDeviceStatus.cpp
    # ServerConnection::openMediaExternally downloads with an Authorization
    # header and opens the local copy, so the downloader comes with it.
    ${CMAKE_SOURCE_DIR}/src/core/MediaDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ThreadFilterModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    # The mesh-vs-SFU choice is made on every join regardless of whether
    # a transport was compiled in, so it is not voice-gated here either.
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceTransportSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/store/LocalCache.cpp
    ${CMAKE_SOURCE_DIR}/src/util/FileLogger.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_identity_provider_url PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_identity_provider_url PRIVATE
    bsfchat_protocol
    # Gui is for IdentityClient's QDesktopServices::openUrl and Multimedia
    # for the audio-device names Settings stores; nothing here opens either.
    # The test is QTEST_GUILESS_MAIN — no QGuiApplication, no window system.
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Sql Qt6::Test
    nlohmann_json::nlohmann_json
)
add_test(NAME test_identity_provider_url COMMAND test_identity_provider_url)

# What each user-visible error surface is allowed to say once the homeserver
# has stopped accepting our token (the 2026-09-19 purge). Drives the real
# ServerConnection: no window, no server — MatrixClient's reply signals are
# emitted by hand and the one genuinely asynchronous path talks to a refused
# port on localhost. Same source list as test_identity_provider_url, for the
# same reason (the BotAdminModel* Q_PROPERTY).
qt_add_executable(test_error_surface_guards
    test_error_surface_guards.cpp
    ${CMAKE_SOURCE_DIR}/src/net/ServerConnection.cpp
    # ServerConnection gained a MediaTicketCache and a MediaDownloader when
    # security/media-tickets merged. This target did not exist on that branch,
    # so it never picked them up — the same cross-branch shape as BotAdminModel.
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MediaDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/model/BotAdminModel.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MatrixClient.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncLoop.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncBackoff.cpp
    ${CMAKE_SOURCE_DIR}/src/net/VoiceSession.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
    ${CMAKE_SOURCE_DIR}/src/identity/IdentityClient.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AudioDeviceStatus.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ThreadFilterModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceTransportSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/store/LocalCache.cpp
    ${CMAKE_SOURCE_DIR}/src/util/FileLogger.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_error_surface_guards PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_error_surface_guards PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Sql Qt6::Test
    nlohmann_json::nlohmann_json
)
add_test(NAME test_error_surface_guards COMMAND test_error_surface_guards)

# WHEN the upload failure signals fire and WHOSE upload they describe, rather
# than what they say. Same sources as test_error_surface_guards above — it is
# the same ServerConnection and the same counter — kept as its own target
# because it pins the two invariants that file does not: a pre-flight failure
# must not be reported before sendMediaMessage has returned to the caller that
# is about to count the upload, and an upload the composer did not start must
# never arrive on mediaSendFailed / mediaSendCompleted at all. Read the header
# of the .cpp for the two composer defects they prevent.
qt_add_executable(test_composer_upload_lock
    test_composer_upload_lock.cpp
    ${CMAKE_SOURCE_DIR}/src/net/ServerConnection.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MediaDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/model/BotAdminModel.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MatrixClient.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncLoop.cpp
    ${CMAKE_SOURCE_DIR}/src/net/SyncBackoff.cpp
    ${CMAKE_SOURCE_DIR}/src/net/VoiceSession.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
    ${CMAKE_SOURCE_DIR}/src/identity/IdentityClient.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AudioDeviceStatus.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ThreadFilterModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceTransportSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/store/LocalCache.cpp
    ${CMAKE_SOURCE_DIR}/src/util/FileLogger.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_composer_upload_lock PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_composer_upload_lock PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Sql Qt6::Test
    nlohmann_json::nlohmann_json
)
add_test(NAME test_composer_upload_lock COMMAND test_composer_upload_lock)

# Publish gate for the QML snapshot lists (DMs, server members, bans): a
# changed list must announce itself, an identical rebuild must stay silent
# (U-H2). Header-only (src/net/SnapshotGate.h), so no ServerConnection needed.
qt_add_executable(test_snapshot_gate test_snapshot_gate.cpp)
target_include_directories(test_snapshot_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_snapshot_gate PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_snapshot_gate COMMAND test_snapshot_gate)

# Channel-list unread dot: first-sight seeding of the read marker, and the
# server-clock rule for everything written to it. Header-only
# (src/core/ReadState.h), so no Settings and no QSettings file.
qt_add_executable(test_read_state test_read_state.cpp)
target_include_directories(test_read_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_read_state PRIVATE
    BSFCHAT_QML_DIR="${CMAKE_SOURCE_DIR}/qml")
target_link_libraries(test_read_state PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_read_state COMMAND test_read_state)

# Add-server URL resolution: normalisation, Matrix .well-known discovery,
# and the honest login-flow outcome that replaced "any error means
# password-only" (which is how `bsfchat.com` produced a registration form
# for a web server). ServerDiscovery takes its HTTP as a std::function, so
# this needs no network, no event loop and no Settings.
# Ownership of the bsfchat:// forwarding socket. The old install() unlinked
# a live instance's socket, so deep links silently went to the newest process.
qt_add_executable(test_url_handler
    test_url_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/UrlHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/AppProfile.cpp
)
target_include_directories(test_url_handler PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_url_handler PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
add_test(NAME test_url_handler COMMAND test_url_handler)

# Correlating an async reply with its request. Qt::SingleShotConnection does
# NOT mean "only one slot runs" — that misreading crossed uploads and DMs over.
qt_add_executable(test_tokened_reply test_tokened_reply.cpp)
target_include_directories(test_tokened_reply PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_tokened_reply PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_tokened_reply COMMAND test_tokened_reply)

# One DM room per peer: the existing-room lookup, the in-flight create guard,
# the m.direct merge that lets the INVITED side recognise a DM at all, and —
# the other half of the same question — that a room recognised as a DM by
# EITHER side is kept out of every one of this server's channel lists.
# DirectRooms.h is header-only; RoomListModel is where the exclusion lives and
# pulls in nothing but Qt Core (ChannelRestore.h and VoiceRoster.h are headers
# too), so this still needs neither the protocol lib nor a network.
qt_add_executable(test_direct_rooms
    test_direct_rooms.cpp
    ${CMAKE_SOURCE_DIR}/src/model/RoomListModel.cpp
)
target_include_directories(test_direct_rooms PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_direct_rooms PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_direct_rooms COMMAND test_direct_rooms)

# Media cache identity. The key used to include the access token, so every
# re-login re-downloaded every image the user had ever seen.
qt_add_executable(test_media_cache_key
    test_media_cache_key.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MediaDownloader.cpp
)
target_include_directories(test_media_cache_key PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Qt6::Gui for QDesktopServices: the downloader gained openDownloaded(), which
# hands a LOCAL file to the desktop so that "open this attachment" never puts a
# media URL in front of the system browser.
target_link_libraries(test_media_cache_key PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
add_test(NAME test_media_cache_key COMMAND test_media_cache_key)

# Deleting a message must be able to fail. redactEvent reported nothing at
# all, so a refused delete looked exactly like an accepted one.
qt_add_executable(test_redaction_result
    test_redaction_result.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MatrixClient.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_redaction_result PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_redaction_result
    PRIVATE Qt6::Core Qt6::Network Qt6::Test bsfchat_protocol nlohmann_json::nlohmann_json)
add_test(NAME test_redaction_result COMMAND test_redaction_result)

# Central 401 detection: one dead-token reply from ANY authenticated endpoint
# has to end the session, and the watcher that spots it must not eat the body
# its call site is about to read. Drives the real MatrixClient against a stub
# homeserver because the ordering hazard only exists against a real reply.
qt_add_executable(test_token_rejection
    test_token_rejection.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MatrixClient.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_token_rejection PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_token_rejection
    PRIVATE Qt6::Core Qt6::Network Qt6::Test bsfchat_protocol nlohmann_json::nlohmann_json)
add_test(NAME test_token_rejection COMMAND test_token_rejection)

# 401 classification: the client had no dead-token handling at all, so an
# expired access token showed as a permanent "Reconnecting…".
qt_add_executable(test_auth_error
    test_auth_error.cpp
    ${CMAKE_SOURCE_DIR}/src/net/AuthError.cpp
)
target_include_directories(test_auth_error PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_auth_error PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_auth_error COMMAND test_auth_error)

# Recovery from a server-side token purge (the 2026-09-19 incident): what a
# "reconnect" is allowed to do once the homeserver has rejected our token,
# and the reversibility that keeps the expired-session surface from latching.
# Header-only (src/net/SessionAuth.h) — no connection, no event loop, no
# network, and deliberately no GUI.
qt_add_executable(test_session_auth test_session_auth.cpp)
target_include_directories(test_session_auth PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_session_auth PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_session_auth COMMAND test_session_auth)

qt_add_executable(test_server_discovery
    test_server_discovery.cpp
    ${CMAKE_SOURCE_DIR}/src/net/ServerDiscovery.cpp
)
target_include_directories(test_server_discovery PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_server_discovery PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_server_discovery COMMAND test_server_discovery)

# Video surface fan-out (VideoStreamRegistry): one decoded stream feeding
# several QML sinks at once. That is what lets a popped-out feed keep its
# in-room tile live, and what makes a fullscreen window a third surface
# rather than a theft. Needs Qt Multimedia for QVideoSink/QVideoFrame but
# no decoder, no capture device, no network and no window.
qt_add_executable(test_video_registry
    test_video_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/video/VideoStreamRegistry.cpp
)
target_include_directories(test_video_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_video_registry PRIVATE
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
add_test(NAME test_video_registry COMMAND test_video_registry)
# QTEST_MAIN builds a QApplication for the multimedia types; offscreen so
# it never opens a window or steals focus from whoever is at the keyboard.
set_tests_properties(test_video_registry PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Source-text guards for QML hazards no test binary can reach (the BSFChat
# QML module lives inside the app). See the file header for the rc.6 story.
qt_add_executable(test_qml_hygiene test_qml_hygiene.cpp)
target_compile_definitions(test_qml_hygiene PRIVATE
    BSFCHAT_QML_DIR="${CMAKE_SOURCE_DIR}/qml"
    BSFCHAT_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_qml_hygiene PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_qml_hygiene COMMAND test_qml_hygiene)

# Bot accounts: the badge cache and its fetch policy, the MANAGE_BOTS bit, the
# two models' badge roles, the management pane's view-model, and source-text
# guards that a bot's one-time access token never reaches a log file or
# persisted state.
#
# No network, no event loop, no GUI — BotAdminModel reaches the server through
# std::function hooks (the seam VoiceSession uses for its transport) so the
# whole dialog can be driven headlessly. The last three cases scan the sources
# instead of exercising them, in the spirit of test_qml_hygiene: a printed
# credential is not something a running test can observe.
qt_add_executable(test_bots
    test_bots.cpp
    ${CMAKE_SOURCE_DIR}/src/model/BotAdminModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MemberListModel.cpp
    ${CMAKE_SOURCE_DIR}/src/model/MessageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/net/MediaTicketCache.cpp
    ${CMAKE_SOURCE_DIR}/src/model/ThreadFilterModel.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MarkdownParser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/MentionRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/util/PermissionMath.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SearchParser.cpp
)
target_include_directories(test_bots PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_bots PRIVATE
    BSFCHAT_QML_DIR="${CMAKE_SOURCE_DIR}/qml"
    BSFCHAT_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_bots PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Test
)
add_test(NAME test_bots COMMAND test_bots)
