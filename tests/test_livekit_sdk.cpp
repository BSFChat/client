// Vendoring gate for the LiveKit C++ client SDK.
//
// Built and run only when -DBSFCHAT_ENABLE_LIVEKIT=ON. There is no
// LiveKit transport yet; this test exists to catch the two ways a
// vendored-prebuilt dependency goes wrong silently:
//
//  1. The runtime libraries are not where the loader expects them.
//     liblivekit resolves liblivekit_ffi lazily at load time (macOS:
//     install name @rpath/liblivekit_ffi.dylib with LC_RPATH
//     @loader_path; Linux: NEEDED liblivekit_ffi.so with RUNPATH
//     $ORIGIN), so a missing ffi library LINKS FINE and only fails when
//     a user joins a call. Upstream's own CMake package does not check
//     for it either — LiveKitTargets-release.cmake lists the ffi
//     library as the bare string "livekit_ffi" in
//     IMPORTED_LINK_DEPENDENT_LIBRARIES and CMake's import-time
//     existence check never sees it.
//
//     Be precise about what this test can and cannot prove here,
//     because it is easy to overstate. Reaching main() does NOT prove
//     the libraries were bundled correctly: CMake automatically adds
//     the vendored deps/ directory to the BUILD rpath, so in the build
//     tree the loader finds them there whether or not the bundling step
//     worked. Verified by deliberately breaking the copy — the binary
//     still ran. The load-time guarantee only bites in a packaged app,
//     where that rpath is gone.
//
//     So the real build-time guarantee is the POST_BUILD assertion in
//     cmake/LiveKit.cmake, which checks the destination paths directly
//     and hard-fails. What this test adds on top is
//     runtimeLibrariesAreCoLocatedWithTheExecutable() below, which
//     asserts co-location from inside the process and therefore holds
//     in both the build tree and a bundle.
//
//  2. deps/ and cmake/LiveKit.cmake disagree about the version, e.g.
//     the pin was bumped but scripts/fetch-livekit-sdk.sh was not
//     re-run. LiveKitSdkProbe.cpp static_asserts that at compile time;
//     this asserts it again at runtime so the failure is legible.
//
// It also records, as executable documentation, the E2EE facts that
// were verified against upstream v1.5.0 sources. If a version bump
// changes any of them, this fails rather than letting a stale
// assumption ride.

#include <QtTest>
#include <QCoreApplication>
#include <QDir>

#include "voice/LiveKitSdkProbe.h"

class TestLiveKitSdk : public QObject {
    Q_OBJECT

private slots:
    // Both libraries must sit next to the executable (or in
    // Contents/Frameworks for a bundle). This is the check that would
    // have caught aom.dll: it does not depend on the loader having
    // found them via some other rpath entry.
    void runtimeLibrariesAreCoLocatedWithTheExecutable() {
        const QDir dir(QCoreApplication::applicationDirPath());
        for (const char* name : {BSFCHAT_LIVEKIT_RUNTIME_NAME,
                                 BSFCHAT_LIVEKIT_FFI_RUNTIME_NAME}) {
            QVERIFY2(dir.exists(QString::fromUtf8(name)),
                     qPrintable(QStringLiteral("%1 is not beside the executable "
                                               "(%2). It would be missing from a "
                                               "package built from this tree.")
                                    .arg(QString::fromUtf8(name), dir.absolutePath())));
        }
    }

    // The exported symbol actually resolved to real code, wherever the
    // loader found it from.
    void linkedSymbolResolves() {
        const LiveKitProbeResult r = probeLiveKitSdk();
        QVERIFY2(r.linkedSymbolOk,
                 "Called an exported liblivekit symbol but got the wrong "
                 "result. The library loaded but is not the one we think.");
    }

    void vendoredHeadersMatchThePin() {
        const LiveKitProbeResult r = probeLiveKitSdk();
        QCOMPARE(r.headerVersion, r.pinnedVersion);
    }

    // Verified against upstream include/livekit/e2ee.h and src/e2ee.cpp
    // at tag v1.5.0, and against the exported symbols of the shipped
    // liblivekit binary. E2EE is genuinely present and FFI-wired.
    //
    // Having the API is NOT the same as having E2EE. LiveKit distributes
    // no keys — upstream docs state it "does not (and cannot) store or
    // transport encryption keys for you" — and BSFChat has no key
    // exchange to supply them. Do not read a green test here as "voice
    // is end-to-end encrypted".
    void e2eeTypesArePresent() {
        const LiveKitProbeResult r = probeLiveKitSdk();
        QVERIFY2(r.e2eeAvailable,
                 "livekit::E2EEOptions / KeyProviderOptions / "
                 "E2EEManager::FrameCryptor did not behave as upstream "
                 "v1.5.0 documents. Re-read e2ee.h before trusting any "
                 "E2EE claim against this SDK version.");
    }

    void e2eeDefaultsToAesGcm() {
        const LiveKitProbeResult r = probeLiveKitSdk();
        QVERIFY(r.e2eeDefaultsToGcm);
    }

    // The ratchet salt default is a PUBLIC, hard-coded constant shared by
    // every LiveKit SDK. It is asserted here because the ratchet's
    // security properties depend on it: RatchetKeyMaterial(current) =
    // KDF(current, ratchet_salt) takes no secret input, so anyone holding
    // key generation N can derive N+1, N+2, ... That is why ratcheting
    // alone cannot exclude a participant who has left a call.
    void defaultRatchetSaltIsThePublicConstant() {
        const LiveKitProbeResult r = probeLiveKitSdk();
        QCOMPARE(r.e2eeDefaultRatchetSalt, QStringLiteral("LKFrameEncryptionKey"));
    }
};

QTEST_MAIN(TestLiveKitSdk)
#include "test_livekit_sdk.moc"
