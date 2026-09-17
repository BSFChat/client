// RTP video actually ARRIVES — end to end, through two real
// PeerConnectionManagers (S-16).
//
// The defect this was written for: screen share and webcam sent RTP
// happily and the receiver never emitted a single frame. Every existing
// test missed it, and the reason is worth remembering:
//
//   * test_media_loopback drives raw rtc::PeerConnections with ONE
//     video m-line. libdatachannel's impl::PeerConnection::dispatchMedia
//     has a shortcut — "if there is exactly one track line, give every
//     incoming media packet to it" — so a single-track harness never
//     exercises the SSRC demultiplexer at all.
//   * test_voice_teardown drives two real managers (two m-lines,
//     vscreen + vcamera) but only pushes video under an env flag, and
//     asserts nothing about reception.
//
// Production has two m-lines. dispatchMedia therefore routes by SSRC,
// through a map built solely from the `a=ssrc:` attributes of the
// negotiated descriptions — and our m-lines carried none, so the map was
// empty and every RTP packet was dropped before any track saw it.
//
// So: both directions, both streams, real DTLS-SRTP over loopback, and
// the assertion is that PeerConnectionManager::videoFrameReceived fires
// on the far side with the bytes we put in.

#include "voice/PeerConnectionManager.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>

#include <cstdio>
#include <functional>
#include <memory>

namespace {

// A synthetic but structurally valid Annex-B access unit: SPS, PPS and
// an IDR slice whose payload is large enough to force FU-A fragmentation
// across ~20 RTP packets, which is the path a real 1080p keyframe takes.
// Nothing decodes it — this test is about transport, not pixels.
QByteArray makeAccessUnit(bool keyframe, int seed)
{
    QByteArray au;
    auto startCode = [&au]() {
        au.append(3, '\0');
        au.append(char(1));
    };
    if (keyframe) {
        startCode();
        for (int b : {0x67, 0x42, 0x00, 0x1E, 0x8D, 0x68, 0x05, 0x00, 0x5B, 0xA1})
            au.append(char(b));
        startCode();
        for (int b : {0x68, 0xCE, 0x3C, 0x80}) au.append(char(b));
    }
    startCode();
    au.append(char(keyframe ? 0x65 : 0x41));   // IDR / non-IDR slice
    const int payload = keyframe ? 24000 : 1200;
    for (int i = 0; i < payload; ++i)
        au.append(char((i * 31 + seed) & 0x7F));   // no accidental start codes
    return au;
}

PeerCaps videoCaps()
{
    PeerCaps caps;
    caps.videoRtp = true;
    caps.controlDc = true;
    caps.videoCodecs = {QStringLiteral("h264")};
    caps.h264ProfilesDecode = {QStringLiteral("cb")};
    caps.h264ProfilesEncode = {QStringLiteral("cb")};
    return caps;
}

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

Peers bringUpPair(const std::function<void(Peers&)>& afterOffer = {})
{
    rtc::Configuration cfg;   // no ICE servers — loopback host candidates
    Peers p;
    p.offerer = new PeerConnectionManager(
        QStringLiteral("@b:test"), QStringLiteral("call-rx"), cfg);
    p.answerer = new PeerConnectionManager(
        QStringLiteral("@a:test"), QStringLiteral("call-rx"), cfg);
    connectSignalling(p.offerer, p.answerer);
    connectSignalling(p.answerer, p.offerer);

    auto failed = std::make_shared<bool>(false);
    for (PeerConnectionManager* pc : {p.offerer, p.answerer}) {
        QObject::connect(pc, &PeerConnectionManager::peerStateChanged, pc,
                         [failed](PeerConnectionManager::PeerState st) {
                             if (st == PeerConnectionManager::PeerState::Failed) *failed = true;
                         });
    }
    p.offerer->setRemoteCaps(videoCaps());
    p.answerer->setRemoteCaps(videoCaps());

    p.offerer->createOffer();
    if (afterOffer) afterOffer(p);
    if (!pumpUntil([&] { return p.offerer->isChannelOpen()
                             && p.answerer->isChannelOpen(); }, 45000)) {
        std::fprintf(stderr, "FAIL: data channels never opened\n");
        return {};
    }
    if (*failed) {
        std::fprintf(stderr, "FAIL: a peer reported Failed during bring-up\n");
        return {};
    }
    p.offerer->ensureVideoTracks();
    // Both ends must have the tracks open before RTP means anything.
    const bool open = pumpUntil([&] {
        for (int s = 0; s < kVideoStreamCount; ++s)
            if (!p.offerer->hasVideoTrackOpen(VideoStreamId(s))
                || !p.answerer->hasVideoTrackOpen(VideoStreamId(s)))
                return false;
        return true;
    }, 15000);
    if (!open) {
        std::fprintf(stderr, "FAIL: video tracks never opened on both ends\n");
        return {};
    }
    return p;
}

// Sends up to `maxFrames` access units on `stream` from `from` and waits
// for `to` to emit videoFrameReceived for that stream.
bool expectDelivery(PeerConnectionManager* from, PeerConnectionManager* to,
                    VideoStreamId stream, const char* label)
{
    int received = 0;
    QByteArray firstReceived;
    auto conn = QObject::connect(to, &PeerConnectionManager::videoFrameReceived,
        to, [&](int idx, const QByteArray& au, bool) {
            if (idx != int(stream)) return;
            if (received == 0) firstReceived = au;
            ++received;
        });

    const int maxFrames = 60;          // 60 x 33 ms of wall clock, worst case
    for (int i = 0; i < maxFrames && received == 0; ++i) {
        EncodedFrame frame;
        frame.data = makeAccessUnit(i % 15 == 0, i);
        frame.keyframe = (i % 15) == 0;
        frame.captureTimeUs = qint64(i) * 33'000;
        frame.width = 1280;
        frame.height = 720;
        from->sendVideoFrame(stream, frame);
        pumpFor(33);
    }
    // Give the last burst time to drain through the pacer and SRTP.
    pumpUntil([&] { return received > 0; }, 3000);
    QObject::disconnect(conn);

    if (from->videoTxFrames(stream) == 0) {
        std::fprintf(stderr, "FAIL (%s): sender never even queued a frame — "
                             "the caps gate rejected it, so this proves nothing\n", label);
        return false;
    }
    if (received == 0) {
        std::fprintf(stderr,
            "FAIL (%s): %llu access units (%llu bytes) went out on the wire and "
            "the receiver got ZERO frames\n", label,
            static_cast<unsigned long long>(from->videoTxFrames(stream)),
            static_cast<unsigned long long>(from->videoTxBytes(stream)));
        return false;
    }
    if (firstReceived.isEmpty()) {
        std::fprintf(stderr, "FAIL (%s): received an EMPTY access unit\n", label);
        return false;
    }
    std::printf("ok (%s): %d frame(s) received, first is %d bytes\n",
                label, received, int(firstReceived.size()));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A control message handed to a peer BEFORE any data channel is open
    // must not be thrown away. This is the shape of the mid-share join:
    // VoiceEngine replays "stream 0 is already running" the moment the
    // peer's caps arrive, which is one SDP round trip before the audio
    // channel opens — the message used to be written to a closed channel
    // and silently dropped, so a viewer that joined during a share never
    // heard that the share existed.
    QByteArray earlyControl;
    Peers p = bringUpPair([&](Peers& peers) {
        QObject::connect(peers.answerer,
                         &PeerConnectionManager::controlMessageReceived,
                         peers.answerer, [&](const QByteArray& json) {
                             if (json.contains("\"stream\"") && earlyControl.isEmpty())
                                 earlyControl = json;
                         });
        peers.offerer->sendControl(
            QByteArrayLiteral("{\"t\":\"stream\",\"stream\":0,\"on\":1}"));
    });
    if (!p.offerer) return 2;

    bool ok = true;
    pumpUntil([&] { return !earlyControl.isEmpty(); }, 5000);
    if (earlyControl.isEmpty()) {
        std::fprintf(stderr, "FAIL (pre-open control): a control message sent "
                             "before the channels opened never arrived\n");
        ok = false;
    } else {
        std::printf("ok (pre-open control): %s\n", earlyControl.constData());
    }

    // The share stream, offerer → answerer: the exact shape of the field
    // report (sender logs "first video AU sent on vscreen", receiver logs
    // nothing at all).
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Screen,
                        "offerer→answerer, vscreen") && ok;
    // The reverse direction rides the same SendRecv m-line but with the
    // answerer's own SSRC, which is negotiated by a different path.
    ok = expectDelivery(p.answerer, p.offerer, VideoStreamId::Screen,
                        "answerer→offerer, vscreen") && ok;
    // The camera m-line proves the demux picks the right track rather
    // than merely finding one.
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Camera,
                        "offerer→answerer, vcamera") && ok;
    ok = expectDelivery(p.answerer, p.offerer, VideoStreamId::Camera,
                        "answerer→offerer, vcamera") && ok;

    delete p.offerer;
    delete p.answerer;
    pumpFor(300);
    rtc::Cleanup().wait();

    if (!ok) return 1;
    std::printf("PASS: RTP video arrives in both directions on both streams\n");
    return 0;
}
