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
// one peer mid-flight while the other keeps sending. It is written to be
// run under AddressSanitizer, where a late callback into the freed object
// is a hard failure rather than a coin flip:
//
//   cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
//       -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" ...
//   ./build-asan/tests/test_voice_teardown
//
// Exit 0 = pass. It is registered as an ordinary ctest too: without ASan
// it still catches a teardown that crashes or hangs outright.

#include "voice/PeerConnectionManager.h"

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
    p.offerer->setRemoteCaps(videoCaps());
    p.answerer->setRemoteCaps(videoCaps());

    p.offerer->createOffer();
    if (!pumpUntil([&] { return p.offerer->isChannelOpen()
                             && p.answerer->isChannelOpen(); }, 15000)) {
        std::fprintf(stderr, "FAIL: data channels never opened (round %d)\n",
                     round);
        return {};
    }
    // Video m-lines were announced in the initial offer; give the tracks
    // a moment to open before the frame storm starts.
    p.offerer->ensureVideoTracks();
    pumpUntil([&] { return p.offerer->hasVideoTrackOpen(VideoStreamId::Screen); },
              5000);
    return p;
}

// Everything the old destructor could be racing: audio frames, JPEG
// screen frames and RTP video, in both directions.
void blast(PeerConnectionManager* from, int iterations)
{
    static const QByteArray au = makeAccessUnit();
    for (int i = 0; i < iterations; ++i) {
        EncodedFrame frame;
        frame.data = au;
        frame.keyframe = (i % 10) == 0;
        frame.captureTimeUs = qint64(i) * 33'000;
        frame.width = 640;
        frame.height = 360;
        from->sendVideoFrame(VideoStreamId::Screen, frame);
        from->sendAudioFrame(QByteArray(160, char(i & 0xFF)));
        from->sendScreenFrame(QByteArray(512, char(0x7F)));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Three rounds: destroying the RECEIVER mid-stream, destroying the
    // SENDER mid-stream, and destroying both at once the way
    // VoiceEngine::stop()'s qDeleteAll does.
    for (int round = 0; round < 3; ++round) {
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
        default:
            // Both at once, no close() in between — the quit shape.
            delete p.offerer;
            delete p.answerer;
            p.offerer = nullptr;
            p.answerer = nullptr;
            break;
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

    // Join libdatachannel's global worker threads before static
    // destruction — otherwise the process can segfault at exit (seen
    // under ctest, where the harness reaps fast).
    rtc::Cleanup().wait();
    std::printf("PASS: peers destroyed mid-flight with no late callback\n");
    return 0;
}
