cmake_minimum_required(VERSION 3.20...3.31)

# Resolve BSFCHAT_VERSION (and its derived strict / NSIS / Android-code
# variants) BEFORE project() because project(VERSION ...) wants the
# strict M.N.P value. cmake/Version.cmake honours an explicit
# -DBSFCHAT_VERSION= override and otherwise reads `git describe`.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)

project(bsfchat-client VERSION ${BSFCHAT_VERSION_DOTS} LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Ccache.cmake)

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

# LiveKit SFU transport. OFF by default and deliberately so: it needs a
# vendored prebuilt SDK in deps/ (scripts/fetch-livekit-sdk.sh), it is
# desktop-only, and the transport itself is not implemented yet. With
# this OFF the build is unchanged in every respect.
option(BSFCHAT_ENABLE_LIVEKIT
    "Build against the vendored LiveKit C++ client SDK (desktop only, \
requires scripts/fetch-livekit-sdk.sh)" OFF)

include(cmake/Dependencies.cmake)
include(cmake/LiveKit.cmake)

# libaom's build (build/cmake/aom_configure.cmake) assumes it is the
# top-level project and appends -D_WIN32_WINNT=0x0601 to the *cached*
# CMAKE_<LANG>_FLAGS_<CONFIG> variables. Pulled in via FetchContent that
# Windows 7 targeting leaks into every target in this build — including
# bsfchat-app, where it hides the Win8+ APIs the Media Foundation video
# backend needs (e.g. MF_LOW_LATENCY, undeclared under _WIN32_WINNT <
# 0x0602). Scrub it out in this (top-level) scope so our targets fall
# back to the SDK default (Win10); libaom keeps 0x0601 via its own
# subdirectory scope.
if(WIN32)
    foreach(_cfg DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
        foreach(_lang C CXX)
            string(REPLACE "-D_WIN32_WINNT=0x0601" ""
                CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
        endforeach()
    endforeach()
endif()

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
    src/core/AppProfile.cpp
    src/core/AudioDeviceStatus.cpp
    src/core/AudioGainSettings.cpp
    src/core/NotificationManager.cpp
    src/core/Settings.cpp
    src/core/TintedIconProvider.cpp
    src/core/MediaDownloader.cpp
    src/core/Updater.cpp
    src/core/Haptics.cpp
    src/core/AndroidPermissions.cpp
    src/core/AndroidNotifier.cpp
    src/core/UrlHandler.cpp
    src/identity/IdentityClient.cpp
    src/identity/IdentityApiClient.cpp
    src/net/MatrixClient.cpp
    src/net/MediaTicketCache.cpp
    src/net/SyncBackoff.cpp
    src/net/AuthError.cpp
    src/net/SyncLoop.cpp
    src/net/ServerConnection.cpp
    src/net/VoiceSession.cpp
    src/net/ServerManager.cpp
    src/net/ServerDiscovery.cpp
    src/net/HttpFetch.cpp
    src/model/ServerListModel.cpp
    src/model/RoomListModel.cpp
    src/model/MessageModel.cpp
    src/model/ThreadFilterModel.cpp
    src/model/MemberListModel.cpp
    src/model/BotAdminModel.cpp
    src/model/ChannelInviteModel.cpp
    src/model/SelfRoleModel.cpp
    src/store/LocalCache.cpp
    src/util/FileLogger.cpp
    src/util/MarkdownParser.cpp
    src/util/MentionRenderer.cpp
    src/util/PermissionMath.cpp
    src/util/SearchParser.cpp
    # NotificationSounds is used by NotificationManager for chat
    # chimes — always needed, not voice-gated. SoundGenerator is
    # its only dep (a tiny pure-DSP helper).
    src/voice/NotificationSounds.cpp
    src/voice/SoundGenerator.cpp
)

if(BSFCHAT_ENABLE_VOICE)
    target_sources(bsfchat-app PRIVATE
        # The transport seam. IVoiceTransport.cpp is empty apart from
        # its include — it exists so AUTOMOC generates the moc for the
        # interface's Q_OBJECT/signals. See the comment in the file.
        src/voice/IVoiceTransport.cpp
        # VoiceEngine is the MESH implementation of IVoiceTransport.
        # Mesh is permanent, not transitional: it is the only transport
        # on Android and the only one that can carry the AV1 lossless
        # screen-share tier.
        src/voice/VoiceEngine.cpp
        src/voice/PeerConnectionManager.cpp
        # Mesh-vs-SFU choice per join. Pure logic, no transport
        # instances — see the header for why a channel must be
        # all-mesh or all-SFU and what the server has to enforce.
        src/voice/VoiceTransportSelector.cpp
        # Media-key parsing and the ONE home for security labelling.
        # Read the header before writing any string about voice security.
        src/voice/VoiceEncryption.cpp
        # "Hide my IP address": the ICE transport-policy decision, and the ONE
        # home for what the UI may claim about it. Same rule as
        # VoiceEncryption.cpp above — read the header before writing any string
        # about whether an address is hidden.
        src/voice/IpPrivacy.cpp
        # AudioEngine is the GUI-thread facade; AudioWorker is the
        # real-time pipeline that runs on the dedicated audio thread,
        # and AudioPacketQueue is the handoff between them.
        src/voice/AudioEngine.cpp
        src/voice/AudioWorker.cpp
        src/voice/AudioPacketQueue.cpp
        src/voice/AudioMixer.cpp
        # Capture AGC + look-ahead limiter (both directions). Pure DSP,
        # unit-tested in test_voice_gain.
        src/voice/VoiceGain.cpp
        # Per-peer receive jitter buffer: reordering, Opus PLC and
        # playout scheduling for the unreliable/unordered audio channel.
        src/voice/JitterBuffer.cpp
        # SoundGenerator + NotificationSounds moved out of this
        # block — they're needed for chat chimes on mobile too.
        # Android: toggle MODE_IN_COMMUNICATION + speakerphone on
        # voice start, restore on stop. No-op on other platforms so
        # the source is unconditionally compiled.
        src/voice/AndroidAudioRouting.cpp
        src/voice/AndroidScreenShareController.cpp
        src/voice/ScreenShareController.cpp
        src/voice/CameraController.cpp
        # Real-video pipeline: RTP H.264 tracks (+ AV1 lossless tier).
        # Codec backends are per-platform; the factory (#ifdef matrix)
        # lives in VideoCodecFactory.cpp.
        src/voice/video/VideoCodecFactory.cpp
        # Latches "this machine cannot actually decode X", overriding a
        # capability probe that said otherwise.
        src/voice/video/VideoDecodeHealth.cpp
        src/voice/video/FrameConverter.cpp
        src/voice/video/VideoSendPipeline.cpp
        src/voice/video/VideoReceivePipeline.cpp
        src/voice/video/VideoStreamRegistry.cpp
        src/voice/video/VideoRateController.cpp
        # macOS-only (not iOS): ScreenCaptureKit / AVCaptureDevice
        # device paths are desktop-shape and rely on Qt Multimedia
        # features (QVideoSink pipelines) that need rework for iOS.
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacScreenCapturer.mm>
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacCameraPermission.mm>
        $<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>:src/voice/MacCameraCapturer.mm>
    )
    target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_VOICE_ENABLED)
    if(BSFCHAT_HAVE_OPENH264)
        target_sources(bsfchat-app PRIVATE
            src/voice/video/OpenH264Encoder.cpp
            src/voice/video/OpenH264Decoder.cpp
        )
        target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_HAVE_OPENH264)
        target_link_libraries(bsfchat-app PRIVATE openh264::openh264)
    endif()
    # AV1 lossless tier (libaom) — desktop platforms.
    if(BSFCHAT_HAVE_AOM)
        target_sources(bsfchat-app PRIVATE
            src/voice/video/AomLosslessEncoder.cpp
            src/voice/video/AomLosslessDecoder.cpp
        )
        target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_HAVE_AOM)
        target_link_libraries(bsfchat-app PRIVATE aom)
        # libaom's CMake is old-style (same story as libyuv below): the
        # `aom` target sets its header paths with directory-scoped
        # include_directories() and exports no usage requirements, so the
        # aom source root (public <aom/*.h> headers) and its build dir
        # (generated aom_config.h) must be added to consumers explicitly.
        target_include_directories(bsfchat-app PRIVATE
            ${libaom_SOURCE_DIR} ${libaom_BINARY_DIR})
    endif()
    # Hardware codec backends: VideoToolbox on desktop macOS, Media
    # Foundation on Windows. Both preferred by the factory, with
    # openh264 / the MS software MFT as the automatic fallbacks.
    if(APPLE AND NOT IOS)
        target_sources(bsfchat-app PRIVATE
            src/voice/video/MacVTEncoder.mm
            src/voice/video/MacVTDecoder.mm
            # Every target that builds VideoReceivePipeline.cpp needs this.
            src/voice/video/LatencyCriticalActivity.mm
        )
        target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_HAVE_VIDEOTOOLBOX)
        target_link_libraries(bsfchat-app PRIVATE
            "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
    elseif(WIN32)
        target_sources(bsfchat-app PRIVATE
            src/voice/video/MFEncoder.cpp
            src/voice/video/MFDecoder.cpp
        )
        target_compile_definitions(bsfchat-app PRIVATE BSFCHAT_HAVE_MEDIAFOUNDATION)
        target_link_libraries(bsfchat-app PRIVATE
            mfplat mfuuid mf mfreadwrite wmcodecdspuuid ole32 oleaut32)
    endif()
endif()

# Android: point Qt at our custom package source dir which carries a
# patched AndroidManifest.xml with softInputMode=adjustResize so the
# on-screen keyboard shrinks the window rather than covering the
# composer.
if(ANDROID)
    set_property(TARGET bsfchat-app PROPERTY
        QT_ANDROID_PACKAGE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/android")
    # androiddeployqt substitutes -- %%INSERT_VERSION_*%% in the
    # manifest from these properties. Keeping them out of the
    # manifest source means the version doesn't drift from the rest
    # of the build.
    set_property(TARGET bsfchat-app PROPERTY
        QT_ANDROID_VERSION_NAME "${BSFCHAT_VERSION}")
    set_property(TARGET bsfchat-app PROPERTY
        QT_ANDROID_VERSION_CODE "${BSFCHAT_VERSION_CODE}")

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
# The user-visible version string. Read by the Updater for its
# semver comparison with GitHub Releases' tag_name. BSFCHAT_VERSION
# may carry a -suffix (e.g. "0.0.0-dev"); BSFCHAT_VERSION_DOTS is the
# strict M.N.P form needed by Apple bundle keys that reject
# non-numeric components. Both come from cmake/Version.cmake.
target_compile_definitions(bsfchat-app PRIVATE
    BSFCHAT_VERSION="${BSFCHAT_VERSION}")

set_target_properties(bsfchat-app PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BSFChat"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${BSFCHAT_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${BSFCHAT_VERSION_DOTS}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${BSFCHAT_VERSION_DOTS}"
    MACOSX_BUNDLE_INFO_STRING "BSFChat"
    MACOSX_BUNDLE_ICON_FILE "BSFChat.icns"
    WIN32_EXECUTABLE TRUE
)

if(APPLE)
    # V-H4. This property is load-bearing twice over: it is the bundle's
    # Info.plist, AND it is what Qt reads at CONFIGURE time to decide
    # whether to link the static QMicrophonePermission /
    # QCameraPermission backends (Qt6CoreMacros.cmake,
    # _qt_internal_darwin_permission_finalizer: no property ⇒ it returns
    # immediately ⇒ no plugin ⇒ every permission query answers Denied,
    # with no OS prompt and no entry in System Settings for the user to
    # flip). rc.7 shipped with the usage descriptions added POST_BUILD,
    # which is far too late for that decision — the bundle read correctly
    # and the microphone was dead.
    #
    # Configured here rather than left to CMake's own substitution pass:
    # that one only fills @MACOSX_BUNDLE_*@ from same-named VARIABLES, so
    # a custom template fed by target properties silently produces an
    # Info.plist with an empty CFBundleIdentifier and no version.
    set(MACOSX_BUNDLE_EXECUTABLE_NAME "bsfchat-app")
    set(MACOSX_BUNDLE_BUNDLE_NAME "BSFChat")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "${BSFCHAT_BUNDLE_ID}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${BSFCHAT_VERSION_DOTS}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${BSFCHAT_VERSION_DOTS}")
    set(MACOSX_BUNDLE_ICON_FILE "BSFChat.icns")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(bsfchat-app PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()

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
    # The usage descriptions and the bsfchat:// URL scheme used to be
    # injected here, POST_BUILD, with plutil and PlistBuddy. They now live
    # in macos/Info.plist.in, because Qt has to be able to READ them at
    # configure time to link the permission plugins (see the
    # MACOSX_BUNDLE_INFO_PLIST property above). Nothing edits the bundle's
    # plist after the fact any more.
    #
    # Guard it: a plist that reaches the bundle without
    # NSMicrophoneUsageDescription is not a cosmetic problem, it is voice
    # silently not working for every user of that build, and the failure
    # is invisible on a machine that already granted the permission to an
    # earlier bundle with the same id.
    add_custom_command(TARGET bsfchat-app POST_BUILD
        COMMAND /usr/libexec/PlistBuddy -c "print NSMicrophoneUsageDescription"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist" > /dev/null
        COMMAND /usr/libexec/PlistBuddy -c "print NSCameraUsageDescription"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist" > /dev/null
        COMMAND /usr/libexec/PlistBuddy -c "print NSScreenCaptureUsageDescription"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist" > /dev/null
        COMMAND /usr/libexec/PlistBuddy -c "print CFBundleURLTypes:0:CFBundleURLSchemes:0"
                "$<TARGET_BUNDLE_DIR:bsfchat-app>/Contents/Info.plist" > /dev/null
        COMMENT "Checking the bundle's TCC usage descriptions and URL scheme"
        VERBATIM
    )
endif()

set_source_files_properties(qml/theme/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(bsfchat-app
    URI BSFChat
    VERSION 1.0
    # NOTE: an earlier draft of Icon.qml used Qt5Compat.GraphicalEffects'
    # ColorOverlay and we declared `IMPORTS Qt5Compat.GraphicalEffects`
    # here so Android deployment would bundle the plugin. The icon
    # path was rewritten to use the C++ TintedIconProvider, but the
    # IMPORTS line was left in — which made every desktop build that
    # didn't ship the module crash on launch with
    # "module Qt5Compat.GraphicalEffects is not installed". Removed.
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
        qml/components/VoiceParticipantList.qml
        qml/components/ParticipantTile.qml
        qml/components/VideoFeedTile.qml
        qml/components/VideoFullscreenWindow.qml
        qml/components/VideoPopoutWindow.qml
        qml/components/Icon.qml
        qml/components/ThemedSwitch.qml
        qml/components/ThemedScrollBar.qml
        qml/components/ThemedComboBox.qml
        qml/components/ThemedCheckBox.qml
        qml/components/ThemedSlider.qml
        qml/components/InfoBanner.qml
        qml/components/MemberList.qml
        qml/components/LoginDialog.qml
        qml/components/ReauthDialog.qml
        qml/components/UserProfileCard.qml
        qml/components/UserSettings.qml
        qml/components/ServerSettings.qml
        qml/components/ChannelSettings.qml
        qml/components/ClientSettings.qml
        qml/components/ForwardDialog.qml
        qml/components/ImageViewer.qml
        qml/components/ShortcutsDialog.qml
        qml/components/SearchPopup.qml
        qml/components/UpdateDialog.qml
        qml/components/UpdatePanel.qml
        qml/components/UpdateChannelSettings.qml
        qml/components/StatusPicker.qml
        qml/components/ThreadPanel.qml
        qml/components/RoleAssignPopup.qml
        qml/components/SelfRolePicker.qml
        qml/components/ToastHost.qml
        qml/components/VideoPlayerCard.qml
        qml/components/LinkPreview.qml
        qml/components/ScreenPickerDialog.qml
        qml/components/BotBadge.qml
        qml/components/BotManagerPane.qml
        qml/components/AddMemberDialog.qml
        qml/theme/Theme.qml
    RESOURCES
        qml/data/EmojiData.js
        # Pure playback maths shared by VideoPlayerCard.qml and
        # tests/qml/tst_playbackmath.qml — the only part of the video
        # player that can be checked without a window and a decoder.
        qml/js/PlaybackMath.js
        # Updater state → wording/severity/progress formatting, shared by
        # UpdatePanel.qml (modal + inline) and tested in
        # tests/qml/tst_updateformat.qml. Imports PlaybackMath.js for the
        # byte formatter, so both must stay in this resource set.
        qml/js/UpdateFormat.js
        # Process-wide OpenGraph unfurl cache. Shared by every LinkPreview
        # delegate — that is the whole point, see the file header — and
        # tested in tests/qml/tst_linkpreviewcache.qml.
        qml/js/LinkPreviewCache.js
        # Pure HTML→metadata helpers for the same unfurl: OpenGraph parsing
        # and bot-challenge detection. Split out of LinkPreview.qml so they
        # cannot depend on a component scope (rc.7 shipped XHR callbacks that
        # outlived their delegate) and so tests/qml/tst_linkpreviewparse.qml
        # can import them.
        qml/js/LinkPreviewParse.js
        # Overlay visibility rules for the message timeline, shared by
        # MessageView.qml and tests/qml/tst_timelineoverlay.qml.
        qml/js/TimelineOverlay.js
        # Grid arithmetic and stage-selection policy for the voice room's
        # video feeds, shared by VoiceRoom.qml, VideoFeedTile.qml and
        # tests/qml/tst_videostage.qml.
        qml/js/VideoStage.js
        # Pop-out / fullscreen window policy for those same feeds: which
        # window owns a feed's fullscreen, what each window opens at, and
        # the one place the three-way "self preview vs remote stream"
        # surface routing is written. Shared by VoiceRoom.qml,
        # VideoFeedTile.qml, VideoPopoutWindow.qml,
        # VideoFullscreenWindow.qml and tests/qml/tst_videowindows.qml.
        qml/js/VideoWindows.js
        # What the profile card's avatar tile shows, and the reset that stops
        # the previously-shown member's picture surviving into the next
        # member's card. Used by UserProfileCard.qml (the one card instanced
        # by both MemberList.qml and MessageView.qml) and tested in
        # tests/qml/tst_profilecardavatar.qml.
        qml/js/ProfileCardAvatar.js
        # Which channel-sidebar row counts as "selected" once the voice
        # connection and the displayed view disagree. Shared by
        # ChannelList.qml and tests/qml/tst_channelselection.qml.
        qml/js/ChannelSelection.js
        # The composer's in-flight upload bookkeeping — the counter U-H6 and
        # both of the fix/composer-upload-* defects were about. Out of
        # MessageInput.qml so tests/qml/tst_uploadtally.qml can pin the rules
        # instead of a C++ transcription of them.
        qml/js/UploadTally.js
        # Row + closed-field label resolution for ThemedComboBox: the one
        # place that knows textRole does not resolve against a QJsonArray
        # Q_PROPERTY (blank Role-overrides dropdown) and that the older
        # Array.isArray path had the mirror-image hole on QVariantList.
        # Shared by ThemedComboBox.qml and
        # tests/qml_models/tst_comboboxmodel.qml.
        qml/js/ComboBoxText.js
        qml/fonts/Geist-Variable.ttf
        qml/fonts/GeistMono-Variable.ttf
        qml/fonts/OFL.txt
        qml/icons/mic.svg
        qml/icons/mic-off.svg
        qml/icons/headphones.svg
        qml/icons/headphones-off.svg
        qml/icons/screen-share.svg
        qml/icons/app-window.svg
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

# Always emit a PDB for the app, Release included — crash triage on
# tester machines is impossible from bare module+offset, and the cost
# is only link time + a .pdb artifact that ships nowhere.
if(MSVC)
    target_compile_options(bsfchat-app PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(bsfchat-app PRIVATE
        $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

target_link_libraries(bsfchat-app PRIVATE
    bsfchat_protocol
    Qt6::Core Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Sql Qt6::Multimedia Qt6::Svg Qt6::Widgets
)

if(BSFCHAT_ENABLE_VOICE)
    target_link_libraries(bsfchat-app PRIVATE datachannel-static opus yuv)
    # libyuv's CMake is old-style — the `yuv` target carries no usage
    # requirements, so its headers must be added explicitly.
    target_include_directories(bsfchat-app PRIVATE ${libyuv_SOURCE_DIR}/include)
endif()

# LiveKit SDK link + runtime bundling. Placed here, BEFORE the macOS
# codesign POST_BUILD step further down, so the dylibs are copied into
# Contents/Frameworks in time to be signed with the rest of the bundle —
# they arrive linker-signed (adhoc) and an unsigned Mach-O inside a
# signed bundle fails both Gatekeeper and notarisation.
if(BSFCHAT_HAVE_LIVEKIT)
    # LiveKitSdkProbe is the only TU that includes an SDK header today.
    # Keeping SDK types in one translation unit is deliberate: the SDK is
    # eight weeks past its first stable release and its README records
    # breaking API changes at v1.0.0, so the blast radius of a version
    # bump must stay small. The eventual LiveKitVoiceTransport.cpp joins
    # it as the second and last such TU.
    target_sources(bsfchat-app PRIVATE src/voice/LiveKitSdkProbe.cpp)
    target_compile_definitions(bsfchat-app PRIVATE
        BSFCHAT_HAVE_LIVEKIT
        BSFCHAT_LIVEKIT_PINNED_VERSION="${BSFCHAT_LIVEKIT_VERSION}")
    target_link_libraries(bsfchat-app PRIVATE livekit::livekit)
    bsfchat_livekit_bundle_runtime(bsfchat-app)
endif()

# ScreenCaptureKit + CoreGraphics back MacScreenCapturer (picker-selected
# capture; CGImage conversion). It deliberately does NOT call
# CGRequestScreenCaptureAccess any more — the picker needs no Screen
# Recording grant and asking for one re-prompted users every launch; see
# src/voice/ScreenCapturePolicy.h.
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

    # Stable dev-signing — TCC grants for Screen Recording, Mic, and
    # Camera are bound to (bundle ID × cdhash × signing identity), so
    # adhoc-signed builds get their grants invalidated on every
    # rebuild (cdhash churns with every recompile). Signing with a
    # stable identity makes the cdhash deterministic per source state,
    # and TCC keeps the grant across rebuilds.
    #
    # Resolution order:
    #   1. Developer ID Application — preferred. Same cert CI uses
    #      for production signing + notarisation. Trusted by macOS;
    #      per-bundle-id TCC grants survive across rebuilds for as
    #      long as the cert is in the keychain (5 years).
    #   2. BSFChat Dev Signer — self-signed fallback for machines
    #      that don't have the Developer ID cert. Created by
    #      scripts/setup-dev-signing.sh.
    #   3. None → adhoc. Every TCC grant burns on every rebuild.
    execute_process(
        COMMAND security find-identity -v -p codesigning
        OUTPUT_VARIABLE _codesign_identities
        ERROR_QUIET)
    set(_dev_sign_identity "")
    if(_codesign_identities MATCHES "\"(Developer ID Application: [^\"]+)\"")
        set(_dev_sign_identity "${CMAKE_MATCH_1}")
        message(STATUS "Stable dev-signing: ${_dev_sign_identity}")
    elseif(_codesign_identities MATCHES "BSFChat Dev Signer")
        set(_dev_sign_identity "BSFChat Dev Signer")
        message(STATUS "Stable dev-signing: BSFChat Dev Signer (self-signed)")
    else()
        message(STATUS "No stable dev-signing identity — builds will be adhoc-signed.")
        message(STATUS "  TCC grants will re-prompt on every rebuild. Fix by")
        message(STATUS "  importing your Developer ID Application cert, or by")
        message(STATUS "  running scripts/setup-dev-signing.sh.")
    endif()
    if(_dev_sign_identity)
        add_custom_command(TARGET bsfchat-app POST_BUILD
            COMMAND codesign --force --deep
                    --sign "${_dev_sign_identity}"
                    --preserve-metadata=entitlements
                    "$<TARGET_BUNDLE_DIR:bsfchat-app>"
            VERBATIM)
    endif()
endif()

if(GAMECHAT_CLIENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
