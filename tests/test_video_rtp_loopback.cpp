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

#include <QRegularExpression>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

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

// A synthetic HEVC access unit: VPS, SPS, PPS and an IDR_W_RADL slice,
// with the TWO-byte NAL headers H.265 uses (type in bits 1..6 of the
// first byte, not the low 5 bits of one byte — the single easiest thing
// to get wrong when porting H.264 code). Payload large enough to force
// FU fragmentation, same as the H.264 helper. Nothing decodes it; this
// test is about the transport picking the right packetizer, the right
// payload type and the right depacketizer.
QByteArray makeH265AccessUnit(bool keyframe, int seed)
{
    QByteArray au;
    auto startCode = [&au]() {
        au.append(3, '\0');
        au.append(char(1));
    };
    auto nalHeader = [&au](int type) {
        au.append(char((type << 1) & 0x7E));   // F=0, type, layerId high bits
        au.append(char(0x01));                 // layerId low | tid+1 = 1
    };
    if (keyframe) {
        startCode(); nalHeader(32);            // VPS
        for (int b : {0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60}) au.append(char(b));
        startCode(); nalHeader(33);            // SPS
        for (int b : {0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xB0}) au.append(char(b));
        startCode(); nalHeader(34);            // PPS
        for (int b : {0xC1, 0x73, 0xD1, 0x89}) au.append(char(b));
    }
    startCode();
    nalHeader(keyframe ? 19 : 1);              // IDR_W_RADL / TRAIL_N
    const int payload = keyframe ? 24000 : 1200;
    for (int i = 0; i < payload; ++i)
        au.append(char((i * 37 + seed) & 0x7F));   // no accidental start codes
    return au;
}

QByteArray makeAu(VideoCodecKind codec, bool keyframe, int seed)
{
    return codec == VideoCodecKind::H265 ? makeH265AccessUnit(keyframe, seed)
                                         : makeAccessUnit(keyframe, seed);
}

PeerCaps videoCaps(bool hevc = false)
{
    PeerCaps caps;
    caps.videoRtp = true;
    caps.controlDc = true;
    caps.videoCodecs = {QStringLiteral("h264")};
    if (hevc) caps.videoCodecs << QStringLiteral("h265");
    caps.h264ProfilesDecode = {QStringLiteral("cb")};
    caps.h264ProfilesEncode = {QStringLiteral("cb")};
    return caps;
}

// Turn an SDP into what an rc.19-and-earlier peer would have produced:
// no H.265 payload type anywhere. Used to put a REAL old peer on the
// answering side of a real connection, which is the one direction
// setOfferH265ForTesting() cannot reach (libdatachannel builds the
// answer itself, by reciprocating our offer's media).
std::string stripH265(const std::string& sdp, int pt)
{
    QString out = QString::fromStdString(sdp);
    const QString n = QString::number(pt);
    // Drop every attribute line that names the payload type...
    out.remove(QRegularExpression(
        QStringLiteral("a=(rtpmap|fmtp|rtcp-fb):%1 [^\r\n]*\r?\n").arg(n)));
    // ...and the payload type itself from the m= line's format list.
    out.replace(QRegularExpression(
        QStringLiteral("(m=video [^\r\n]*?) %1(?=[ \r\n])").arg(n)),
        QStringLiteral("\\1"));
    return out.toStdString();
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

using SdpFilter = std::function<std::string(const std::string&)>;

// Counts every local description either side emits. After bring-up it
// must stay put: a codec switch that renegotiated would move it, and
// the entire point of two payload types per m-line is that it cannot.
int g_localDescriptions = 0;

void connectSignalling(PeerConnectionManager* a, PeerConnectionManager* b,
                       SdpFilter filter = {})
{
    QObject::connect(a, &PeerConnectionManager::localDescriptionReady, b,
        [b, filter](const std::string& type, const std::string& rawSdp) {
            ++g_localDescriptions;
            const std::string sdp = filter ? filter(rawSdp) : rawSdp;
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

struct BringUpOptions {
    std::function<void(Peers&)> afterOffer;
    // Caps each side believes the OTHER advertises.
    PeerCaps offererSeesAnswerer = videoCaps();
    PeerCaps answererSeesOfferer = videoCaps();
    // Applied to what the answerer sends back to the offerer — this is
    // how an "old peer answering" is simulated.
    SdpFilter answerFilter;
    // Called with every offer the offering side emits, so a test can
    // assert on what actually went out rather than on what it meant.
    std::function<void(const std::string&)> offerAudit;
};

Peers bringUpPair(const BringUpOptions& opts = {})
{
    rtc::Configuration cfg;   // no ICE servers — loopback host candidates
    // Pin ICE to loopback. On GitHub's hosted runners (several interfaces,
    // IPv6, docker bridges) libjuice's default 'any' bind let two in-process
    // peers gather candidates on addresses that never connect to each other,
    // and the handshake waited out its full timeout — on Linux and macOS,
    // on changes unrelated to this test. Locally it always passed.
    cfg.bindAddress = "127.0.0.1";
    Peers p;
    p.offerer = new PeerConnectionManager(
        QStringLiteral("@b:test"), QStringLiteral("call-rx"), cfg);
    p.answerer = new PeerConnectionManager(
        QStringLiteral("@a:test"), QStringLiteral("call-rx"), cfg);
    SdpFilter offerTap;
    if (opts.offerAudit) {
        auto audit = opts.offerAudit;
        offerTap = [audit](const std::string& sdp) { audit(sdp); return sdp; };
    }
    connectSignalling(p.offerer, p.answerer, offerTap);
    connectSignalling(p.answerer, p.offerer, opts.answerFilter);

    auto failed = std::make_shared<bool>(false);
    for (PeerConnectionManager* pc : {p.offerer, p.answerer}) {
        QObject::connect(pc, &PeerConnectionManager::peerStateChanged, pc,
                         [failed](PeerConnectionManager::PeerState st) {
                             if (st == PeerConnectionManager::PeerState::Failed) *failed = true;
                         });
    }
    p.offerer->setRemoteCaps(opts.offererSeesAnswerer);
    p.answerer->setRemoteCaps(opts.answererSeesOfferer);

    p.offerer->createOffer();
    if (opts.afterOffer) opts.afterOffer(p);
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
                    VideoStreamId stream, const char* label,
                    VideoCodecKind codec = VideoCodecKind::H264)
{
    int received = 0;
    QByteArray firstReceived;
    int wrongCodec = 0;
    auto conn = QObject::connect(to, &PeerConnectionManager::videoFrameReceived,
        to, [&](int idx, const QByteArray& au, bool, int rxCodec) {
            if (idx != int(stream)) return;
            if (received == 0) firstReceived = au;
            // The receiver derives the codec from the payload type the
            // packets carried, so this is the assertion that the right
            // PT went on the wire AND the right depacketizer saw it.
            if (VideoCodecKind(rxCodec) != codec) ++wrongCodec;
            ++received;
        });

    const int maxFrames = 60;          // 60 x 33 ms of wall clock, worst case
    for (int i = 0; i < maxFrames && received == 0; ++i) {
        EncodedFrame frame;
        frame.codec = codec;
        frame.data = makeAu(codec, i % 15 == 0, i);
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
    if (wrongCodec > 0) {
        std::fprintf(stderr, "FAIL (%s): %d of %d access units arrived under "
                             "the wrong payload type (expected %s)\n",
                     label, wrongCodec, received, videoCodecName(codec));
        return false;
    }
    std::printf("ok (%s): %d %s frame(s) received, first is %d bytes\n",
                label, received, videoCodecName(codec),
                int(firstReceived.size()));
    return true;
}

// One full bring-up against a peer that has never heard of H.265, in
// whichever offer direction is asked for, with H.264 video proved to
// flow both ways afterwards.
//
//   oldPeerOffers=true  — the OLD side offers. setOfferH265ForTesting()
//                         makes the offering process build exactly the
//                         one-PT m-lines rc.19 built; our answer then
//                         reciprocates that single PT.
//   oldPeerOffers=false — WE offer both, and the old peer's answer comes
//                         back with the H.265 PT stripped out. The
//                         answer is built inside libdatachannel, so the
//                         only faithful way to age it is to edit the SDP
//                         on the wire, which is what an old build would
//                         genuinely have sent.
bool oldPeerInterop(bool oldPeerOffers)
{
    const char* label = oldPeerOffers ? "old peer offers" : "old peer answers";
    BringUpOptions opts;
    // An old build advertises no h265 in its caps either — belt as well
    // as braces, and it is what the selection rule reads.
    opts.offererSeesAnswerer = videoCaps(/*hevc=*/false);
    opts.answererSeesOfferer = videoCaps(/*hevc=*/false);

    if (oldPeerOffers) {
        PeerConnectionManager::setOfferH265ForTesting(false);
    } else {
        opts.answerFilter = [](const std::string& sdp) {
            // Both video m-lines' H.265 payload types (98 and 99).
            return stripH265(stripH265(sdp, 98), 99);
        };
    }

    // A test that silently stopped simulating an old peer would pass
    // for the wrong reason forever, so both mechanisms self-check.
    bool sawOneCodecSdp = false;
    if (oldPeerOffers) {
        opts.answerFilter = {};
        opts.offerAudit = [&sawOneCodecSdp](const std::string& sdp) {
            sawOneCodecSdp = sdp.find("H265") == std::string::npos
                          && sdp.find("H264") != std::string::npos;
        };
    } else {
        auto strip = opts.answerFilter;
        opts.answerFilter = [strip, &sawOneCodecSdp](const std::string& sdp) {
            const std::string out = strip(sdp);
            if (sdp.find("H265") != std::string::npos
                && out.find("H265") == std::string::npos)
                sawOneCodecSdp = true;
            return out;
        };
    }

    Peers p = bringUpPair(opts);
    PeerConnectionManager::setOfferH265ForTesting(true);
    if (!sawOneCodecSdp) {
        std::fprintf(stderr, "FAIL (%s): no H.264-only SDP was ever produced — "
                             "this case proves nothing\n", label);
        delete p.offerer;
        delete p.answerer;
        return false;
    }
    if (!p.offerer) {
        std::fprintf(stderr, "FAIL (%s): the pair never came up\n", label);
        return false;
    }

    bool ok = true;
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Screen,
                        oldPeerOffers ? "old-offers: offerer→answerer, vscreen"
                                      : "old-answers: offerer→answerer, vscreen")
         && ok;
    ok = expectDelivery(p.answerer, p.offerer, VideoStreamId::Screen,
                        oldPeerOffers ? "old-offers: answerer→offerer, vscreen"
                                      : "old-answers: answerer→offerer, vscreen")
         && ok;
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Camera,
                        oldPeerOffers ? "old-offers: offerer→answerer, vcamera"
                                      : "old-answers: offerer→answerer, vcamera")
         && ok;

    delete p.offerer;
    delete p.answerer;
    pumpFor(300);
    if (ok) std::printf("ok (%s): H.264 unaffected in both directions\n", label);
    return ok;
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
    BringUpOptions opts;
    // Both ends decode H.265 in this scenario.
    opts.offererSeesAnswerer = videoCaps(/*hevc=*/true);
    opts.answererSeesOfferer = videoCaps(/*hevc=*/true);
    opts.afterOffer = [&](Peers& peers) {
        QObject::connect(peers.answerer,
                         &PeerConnectionManager::controlMessageReceived,
                         peers.answerer, [&](const QByteArray& json) {
                             if (json.contains("\"stream\"") && earlyControl.isEmpty())
                                 earlyControl = json;
                         });
        peers.offerer->sendControl(
            QByteArrayLiteral("{\"t\":\"stream\",\"stream\":0,\"on\":1}"));
    };
    Peers p = bringUpPair(opts);
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

    // ---- S-18: H.265 on the same negotiated m-lines -----------------
    //
    // No renegotiation happened between the H.264 cases above and these:
    // the H.265 payload type was in the first offer, so switching is a
    // different packetizer and a different PT byte.
    const int sdpsBeforeHevc = g_localDescriptions;

    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Screen,
                        "offerer→answerer, vscreen, H.265",
                        VideoCodecKind::H265) && ok;
    ok = expectDelivery(p.answerer, p.offerer, VideoStreamId::Screen,
                        "answerer→offerer, vscreen, H.265",
                        VideoCodecKind::H265) && ok;
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Camera,
                        "offerer→answerer, vcamera, H.265",
                        VideoCodecKind::H265) && ok;

    // Switch back, on the same track, mid-stream. This is the shape of
    // an H.264-only viewer joining a call that was running H.265.
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Screen,
                        "offerer→answerer, vscreen, back to H.264",
                        VideoCodecKind::H264) && ok;
    // ...and forward again, because a switch must be repeatable: the
    // shared packetization config is mutated in place, and a one-way
    // door here would only show up on the second transition.
    ok = expectDelivery(p.offerer, p.answerer, VideoStreamId::Screen,
                        "offerer→answerer, vscreen, H.265 again",
                        VideoCodecKind::H265) && ok;

    if (g_localDescriptions != sdpsBeforeHevc) {
        std::fprintf(stderr, "FAIL (no-renegotiation): %d local description(s) "
                             "were emitted while switching codec — the whole "
                             "point of two payload types per m-line is that "
                             "NONE are\n", g_localDescriptions - sdpsBeforeHevc);
        ok = false;
    } else {
        std::printf("ok (no-renegotiation): H.264→H.265→H.264→H.265 mid-call "
                    "with zero SDP exchanges\n");
    }

    // The sender must also refuse to put H.265 on the wire toward a peer
    // whose caps say H.264 only — the backstop under the codec selector,
    // for the window in which a joiner's caps have landed but the
    // encoder session has not yet been rebuilt.
    {
        p.offerer->setRemoteCaps(videoCaps(/*hevc=*/false));
        const quint64 before = p.offerer->videoTxFrames(VideoStreamId::Screen);
        EncodedFrame frame;
        frame.codec = VideoCodecKind::H265;
        frame.data = makeH265AccessUnit(true, 1);
        frame.keyframe = true;
        frame.captureTimeUs = 0;
        p.offerer->sendVideoFrame(VideoStreamId::Screen, frame);
        pumpFor(50);
        if (p.offerer->videoTxFrames(VideoStreamId::Screen) != before) {
            std::fprintf(stderr, "FAIL (h265 caps gate): an H.265 frame went "
                                 "out to a peer that decodes only H.264\n");
            ok = false;
        } else {
            std::printf("ok (h265 caps gate): H.265 toward an H.264-only peer "
                        "is dropped before the wire\n");
        }
        p.offerer->setRemoteCaps(videoCaps(/*hevc=*/true));
    }

    delete p.offerer;
    delete p.answerer;
    pumpFor(300);
    rtc::Cleanup().wait();

    // ---- S-18: interop with a peer that only knows H.264 -------------
    //
    // Both offer directions, because they fail differently: an old peer
    // OFFERING gives us a one-PT m-line to answer, while an old peer
    // ANSWERING gives us a one-PT answer to our two-PT offer. Neither
    // may break H.264, which is all such a peer ever receives.
    ok = oldPeerInterop(/*oldPeerOffers=*/true) && ok;
    ok = oldPeerInterop(/*oldPeerOffers=*/false) && ok;

    if (!ok) return 1;
    std::printf("PASS: RTP video arrives in both directions on both streams, "
                "in both codecs, and against an H.264-only peer\n");
    return 0;
}
