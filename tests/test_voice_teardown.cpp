// Teardown safety for PeerConnectionManager — V-C2 / S-6.
//
// The defect: every libdatachannel callback the class installs captures
// raw `this`, `close()` is asynchronous, and VoiceEngine::stop() destroys
// peers synchronously with qDeleteAll(m_peers). Between "close() called"
// and "object freed" the library could still deliver a frame, a message
// or a state change into freed memory. Leave, channel switch, quit, and
// dead-peer cleanup all hit that window, and it is widest while video is
// flowing because onFrame fires per reassembled access unit.
//
// This test drives two REAL PeerConnectionManagers over an in-process
// loopback (no server, no signalling, no codecs — the AU bytes are
// nonsense that only has to survive packetize → SRTP → depacketize), gets
// audio, JPEG and RTP video moving in both directions, and then destroys
// one peer mid-flight while the other keeps sending, in four shapes
// including "deleted from inside the delivery of its own frame".
//
// WHAT IT DOES AND DOES NOT PROVE — read this before trusting it.
// It does NOT reproduce the pre-fix use-after-free: with
// resetAllCallbacks() removed from the destructor it still passes, six
// runs out of six under ASan. In this harness libdatachannel's own
// teardown (the PeerConnection destructor closes and joins) happens to
// close the window before the memory is reused, so "no ASan report" here
// is not evidence that the callbacks were detached. The fix stands on the
// reasoning in resetAllCallbacks(), not on this test going green.
//
// What it IS worth: a regression net for a teardown that crashes, hangs
// or double-frees outright, the ASan+UBSan harness Phase 0 asked for, and
// — with BSFCHAT_TEARDOWN_STRESS_VIDEO=1 — an actual reproducer for a
// libdatachannel defect (see below). If you can make it fail with the fix
// reverted, tighten it and delete this paragraph.
//
// Run under AddressSanitizer:
//
//   cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
//       -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" ...
//   ./build-asan/tests/test_voice_teardown
//
// Exit 0 = pass. It is registered as an ordinary ctest too: without ASan
// it still catches a teardown that crashes or hangs outright.
//
// RTP VIDEO IS OPT-IN (BSFCHAT_TEARDOWN_STRESS_VIDEO=1), and not because
// our side is unsafe with it. Pushing encoded video through the teardown
// trips a separate defect INSIDE libdatachannel v0.24.5: PacingHandler
// schedules
// itself on the global thread pool with
//
//     weak_bind(&PacingHandler::run, this, send)
//
// where `send` is bound as `const message_callback&` — a REFERENCE into
// the media-handler chain the track owns. weak_bind protects `this` but
// not that reference, so a pacing tick that comes due after the track has
// gone calls through a dangling std::function (SEGV at
// pacinghandler.cpp:42, address 0x8). It reproduces in roughly one run in
// three under ASan — 2 runs in 3 before we started clearing the chain,
// roughly 1 in 3 after — and it is not reachable from our callbacks at
// all; clearing the chain in resetAllCallbacks() is as close as we can
// get from this side. Keeping it opt-in keeps ctest
// deterministic; the flag is there to reproduce the upstream bug on
// demand. Belongs with the pacer work (S-15, workstream C) or an upstream
// bump.
//
// The default path still destroys peers with audio, JPEG screen frames
// and control traffic in flight, across all four destruction shapes,
// which is what exercises OUR callback lifetimes.

#include "voice/PeerConnectionManager.h"
#include <memory>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

namespace {

// Minimal plausible Annex-B access unit: SPS/PPS/IDR NALs with long
// start codes. Nothing decodes it.
QByteArray makeAccessUnit()
{
    QByteArray au;
    auto putNal = [&au](std::initializer_list<int> bytes) {
        au.append(4, '\0');
        au[au.size() - 1] = char(1);
        for (int b : bytes) au.append(char(b));
    };
    putNal({0x67, 0x42, 0x00, 0x1E, 0x8D, 0x68, 0x05, 0x00, 0x5B, 0xA1});
    putNal({0x68, 0xCE, 0x3C, 0x80});
    putNal({0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0xFE, 0xF6, 0xF0, 0xFE,
            0x05, 0x36, 0x56, 0x04, 0x50, 0x96, 0x7B, 0x3C, 0x50, 0xFF});
    return au;
}

PeerCaps videoCaps()
{
    PeerCaps caps;
    caps.videoRtp = true;
    caps.videoCodecs = {QStringLiteral("h264")};
    caps.h264ProfilesDecode = {QStringLiteral("cb")};
    caps.h264ProfilesEncode = {QStringLiteral("cb")};
    return caps;
}

// Pump the event loop until `predicate` holds or the deadline expires.
template <typename Predicate>
bool pumpUntil(Predicate predicate, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void pumpFor(int ms)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// Cross-wires two managers so each one's local description and
// candidates reach the other, exactly as the Matrix timeline would.
void connectSignalling(PeerConnectionManager* a, PeerConnectionManager* b)
{
    QObject::connect(a, &PeerConnectionManager::localDescriptionReady, b,
        [b](const std::string& type, const std::string& sdp) {
            if (type == "offer") {
                if (b->initialNegotiationDone()) b->applyNegotiateOffer(sdp);
                else b->applyOffer(sdp);
            } else {
                if (b->initialNegotiationDone()) b->applyNegotiateAnswer(sdp);
                else b->applyAnswer(sdp);
            }
        });
    QObject::connect(a, &PeerConnectionManager::localCandidateReady, b,
        [b](const std::string& candidate, const std::string& mid) {
            b->addRemoteCandidate(candidate, mid);
        });
}

struct Peers {
    PeerConnectionManager* offerer = nullptr;
    PeerConnectionManager* answerer = nullptr;
};

// Brings up a connected pair with video tracks open on the offerer.
Peers bringUpPair(int round)
{
    rtc::Configuration cfg;  // no ICE servers — loopback host candidates
    Peers p;
    p.offerer = new PeerConnectionManager(
        QStringLiteral("@b:test"), QStringLiteral("call-%1").arg(round), cfg);
    p.answerer = new PeerConnectionManager(
        QStringLiteral("@a:test"), QStringLiteral("call-%1").arg(round), cfg);
    connectSignalling(p.offerer, p.answerer);
    connectSignalling(p.answerer, p.offerer);
    // Neither side may report Failed while the pair comes up. This harness
    // has no VoiceEngine to remove a failed peer, so a failure here used to
    // be invisible: applyOffer threw on a redundant second answer, the pair
    // connected anyway, and the test passed — while in the app the engine
    // removed the "failed" answerer and its answer with it (rc.13).
    auto failed = std::make_shared<bool>(false);
    for (PeerConnectionManager* pc : {p.offerer, p.answerer}) {
        QObject::connect(pc, &PeerConnectionManager::peerStateChanged, pc,
                         [failed](PeerConnectionManager::PeerState st) { if (st == PeerConnectionManager::PeerState::Failed) *failed = true; });
    }
    p.offerer->setRemoteCaps(videoCaps());
    p.answerer->setRemoteCaps(videoCaps());

    p.offerer->createOffer();
    if (!pumpUntil([&] { return p.offerer->isChannelOpen()
                             && p.answerer->isChannelOpen(); }, 15000)) {
        std::fprintf(stderr, "FAIL: data channels never opened (round %d)\n",
                     round);
        return {};
    }
    if (*failed) {
        std::fprintf(stderr, "FAIL: a peer reported Failed during bring-up "
                             "(round %d) — negotiation is throwing\n", round);
        return {};
    }
    // Video m-lines were announced in the initial offer. The tracks are
    // opened either way — an open track with an onFrame handler is part of
    // what the destructor has to detach — but only fed when the upstream
    // pacer defect is being reproduced deliberately.
    p.offerer->ensureVideoTracks();
    pumpUntil([&] { return p.offerer->hasVideoTrackOpen(VideoStreamId::Screen); },
              5000);
    return p;
}

bool stressVideo()
{
    return qEnvironmentVariable("BSFCHAT_TEARDOWN_STRESS_VIDEO") == "1";
}

// Everything the old destructor could be racing: audio frames, JPEG
// screen frames and — when asked for — RTP video, in both directions.
void blast(PeerConnectionManager* from, int iterations)
{
    static const QByteArray au = makeAccessUnit();
    const bool video = stressVideo();
    for (int i = 0; i < iterations; ++i) {
        if (video) {
            EncodedFrame frame;
            frame.data = au;
            frame.keyframe = (i % 10) == 0;
            frame.captureTimeUs = qint64(i) * 33'000;
            frame.width = 640;
            frame.height = 360;
            from->sendVideoFrame(VideoStreamId::Screen, frame);
        }
        from->sendAudioFrame(QByteArray(160, char(i & 0xFF)));
        from->sendScreenFrame(QByteArray(512, char(0x7F)));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Four rounds: destroying the RECEIVER mid-stream, destroying the
    // SENDER mid-stream, destroying both at once the way
    // VoiceEngine::stop()'s qDeleteAll does, and — the tightest window —
    // destroying a peer from inside the delivery of one of its own
    // frames, which is when libdatachannel's thread is demonstrably in
    // the middle of calling into this object.
    for (int round = 0; round < 4; ++round) {
        Peers p = bringUpPair(round);
        if (!p.offerer) return 2;

        blast(p.offerer, 40);
        blast(p.answerer, 20);

        switch (round) {
        case 0:
            // Receiver dies while frames are in flight toward it. This is
            // the leave / dead-peer-cleanup shape.
            delete p.answerer;
            p.answerer = nullptr;
            blast(p.offerer, 40);
            break;
        case 1:
            // Sender dies mid-send: its own onFrame/onMessage callbacks
            // for the reverse direction are live at that moment.
            delete p.offerer;
            p.offerer = nullptr;
            blast(p.answerer, 40);
            break;
        case 2:
            // Both at once, no close() in between — the quit shape.
            delete p.offerer;
            delete p.answerer;
            p.offerer = nullptr;
            p.answerer = nullptr;
            break;
        default: {
            // Destroy the receiver from the delivery of its own frame.
            // The signal is emitted through a queued invocation, so the
            // delete lands on this thread while libdatachannel's thread
            // is still pumping into the same object — the exact window
            // resetCallbacks() closes. VoiceEngine's dead-peer cleanup
            // has this shape: a state change arrives, and the peer is
            // deleted from the slot it woke.
            PeerConnectionManager* victim = p.answerer;
            QObject::connect(victim, &PeerConnectionManager::audioFrameReceived,
                             victim, [&p, victim]() {
                if (p.answerer != victim) return;   // already gone
                p.answerer = nullptr;
                delete victim;
            });
            for (int i = 0; i < 200 && p.answerer; ++i) {
                p.offerer->sendAudioFrame(QByteArray(160, char(i & 0xFF)));
                p.offerer->sendScreenFrame(QByteArray(512, char(0x7F)));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            if (p.answerer) {
                std::fprintf(stderr,
                    "FAIL: no audio frame ever arrived — the teardown window "
                    "was never opened, so this round proved nothing\n");
                return 2;
            }
            blast(p.offerer, 40);
            break;
        }
        }

        // Give libdatachannel's worker threads time to try to deliver
        // into whatever was just freed. Without resetCallbacks() before
        // close(), this is the window where ASan reports the
        // use-after-free.
        pumpFor(400);

        delete p.offerer;
        delete p.answerer;
        pumpFor(200);
        std::printf("round %d: teardown clean\n", round);
    }

    // Let every pacing tick libdatachannel scheduled for a track we just
    // destroyed come due while we are still pumping. rtc::Cleanup() joins
    // the thread pool and FORCES the remaining queued tasks to run, so
    // draining first is what makes the exit deterministic rather than a
    // race against v0.24.5's PacingHandler (see the note below).
    pumpFor(1500);

    // Join libdatachannel's global worker threads before static
    // destruction — otherwise the process can segfault at exit (seen
    // under ctest, where the harness reaps fast).
    rtc::Cleanup().wait();
    std::printf("PASS: peers destroyed mid-flight with no late callback%s\n",
                stressVideo() ? " (with RTP video)" : "");
    return 0;
}
