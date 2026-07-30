#include "voice/LiveKitSdkProbe.h"

#include <livekit/build.h>
#include <livekit/e2ee.h>

#include <string>

// The pinned version comes from cmake/LiveKit.cmake. A mismatch means
// deps/ and the build system disagree — e.g. the version was bumped in
// CMake but scripts/fetch-livekit-sdk.sh was never re-run. Fail at
// compile time rather than shipping headers from one release against
// binaries from another.
#ifndef BSFCHAT_LIVEKIT_PINNED_VERSION
#error "BSFCHAT_LIVEKIT_PINNED_VERSION not defined — cmake/LiveKit.cmake must set it"
#endif

static_assert(std::string_view{LIVEKIT_BUILD_VERSION} ==
                  std::string_view{BSFCHAT_LIVEKIT_PINNED_VERSION},
              "Vendored LiveKit headers do not match the version pinned in "
              "cmake/LiveKit.cmake. Re-run scripts/fetch-livekit-sdk.sh.");

LiveKitProbeResult probeLiveKitSdk() {
    LiveKitProbeResult r;
    r.headerVersion = QString::fromUtf8(LIVEKIT_BUILD_VERSION);
    r.pinnedVersion = QString::fromUtf8(BSFCHAT_LIVEKIT_PINNED_VERSION);

    // --- E2EE type availability (header-only) ------------------------
    //
    // Verified against upstream include/livekit/e2ee.h and src/e2ee.cpp
    // at tag v1.5.0: E2EEOptions defaults encryption_type to GCM, and
    // KeyProviderOptions defaults shared_key to nullopt with the salt
    // set from the public kDefaultRatchetSalt constant.
    const livekit::E2EEOptions opts;
    r.e2eeDefaultsToGcm = (opts.encryption_type == livekit::EncryptionType::GCM);
    r.e2eeDefaultRatchetSalt = QString::fromUtf8(
        reinterpret_cast<const char*>(opts.key_provider_options.ratchet_salt.data()),
        static_cast<int>(opts.key_provider_options.ratchet_salt.size()));
    const bool optionsSane =
        !opts.key_provider_options.shared_key.has_value() &&
        !opts.key_provider_options.ratchet_salt.empty() &&
        opts.key_provider_options.ratchet_window_size == livekit::kDefaultRatchetWindowSize;

    // --- Exported-symbol call (forces the dylib/DLL to be present) ---
    //
    // E2EEManager::FrameCryptor's constructor and its three accessors
    // are exported from liblivekit and, unlike setEnabled/setKeyIndex,
    // make no FFI request — they only store and read fields (verified
    // in upstream src/e2ee.cpp). That makes this the one E2EE entry
    // point that can be called safely with no Room, no connection and
    // no key material, which is exactly what a load-time probe wants.
    //
    // A room_handle of 0 is never dereferenced on these paths.
    livekit::E2EEManager::FrameCryptor cryptor(/*room_handle=*/0,
                                              /*participant_identity=*/"bsfchat-probe",
                                              /*key_index=*/3,
                                              /*enabled=*/true);
    r.linkedSymbolOk = (cryptor.participantIdentity() == "bsfchat-probe") &&
                       (cryptor.keyIndex() == 3) && cryptor.enabled();

    r.e2eeAvailable = optionsSane && r.linkedSymbolOk;
    return r;
}
