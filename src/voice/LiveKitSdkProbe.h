#pragma once

// Compile/link/load probe for the vendored LiveKit C++ client SDK.
//
// This is NOT the LiveKit transport. It exists so a build configured
// with -DBSFCHAT_ENABLE_LIVEKIT=ON proves four things that are
// otherwise only proven the first time a user joins a voice call:
//
//   1. the vendored headers are found and compile under our -std=c++20
//      and warning settings,
//   2. the vendored headers' version matches the version CMake pinned,
//   3. the app links against liblivekit and resolves real exported
//      symbols from it (not just header-inline code),
//   4. liblivekit AND its liblivekit_ffi dependency are actually
//      loadable at process start from where the bundling step put them.
//
// (4) is the important one. liblivekit resolves livekit_ffi lazily at
// load time via @rpath/$ORIGIN, so a missing ffi library links cleanly
// and fails only at runtime — the aom.dll shape of bug. A test binary
// that calls probeLiveKitSdk() cannot even start if the libraries are
// not where they should be, which converts that class of failure into a
// red test.
//
// Everything here is deliberately side-effect-free: no FFI requests, no
// network, no audio device, no Room. See LiveKitSdkProbe.cpp.
//
// NAMING, because it matters here more than usual: the `e2ee*` fields
// below carry UPSTREAM's name for the API (livekit::E2EEOptions,
// E2EEManager). They record what the vendored SDK offers. They are not a
// statement about BSFChat, and the way BSFChat uses that API — a shared
// key generated and held by the BSFChat server — is NOT end-to-end
// encryption. Nothing in this struct may be surfaced to a user or
// paraphrased into a label. Read voice/VoiceEncryption.h first.

#include <QString>

struct LiveKitProbeResult {
    // Version string from the vendored <livekit/build.h>.
    QString headerVersion;
    // Version CMake pinned (BSFCHAT_LIVEKIT_VERSION).
    QString pinnedVersion;
    // True when a symbol exported by liblivekit was called successfully.
    bool linkedSymbolOk = false;
    // True when the E2EE types (livekit::E2EEOptions, KeyProviderOptions,
    // E2EEManager::FrameCryptor) are present and behave as documented.
    // Asserted rather than assumed: server/docs/livekit-migration.md
    // §10.6 listed E2EE as UNVERIFIED, and this project has already
    // shipped an invented dependency rationale once.
    bool e2eeAvailable = false;
    // GCM is upstream's default and recommended EncryptionType.
    bool e2eeDefaultsToGcm = false;
    // Default ratchet salt is a PUBLIC constant ("LKFrameEncryptionKey").
    // Recorded because the ratchet's security properties depend on it.
    QString e2eeDefaultRatchetSalt;
};

// Runs the probe. Never throws; never touches the FFI.
LiveKitProbeResult probeLiveKitSdk();
