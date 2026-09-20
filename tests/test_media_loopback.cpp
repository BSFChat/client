// Loopback proof for the H.264-over-RTP media path: two in-process
// rtc::PeerConnections wired back-to-back with the SAME media-handler
// chain PeerConnectionManager builds, one Annex-B access unit pushed
// through. If onFrame doesn't deliver the AU on the far side, the
// receive path is broken at the library/chain level — no network, no
// codecs, no signaling server involved. Exit 0 = pass.

#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// Every wait on a real handshake (ICE over loopback + DTLS + SCTP) uses this.
// 10 s was enough on a developer machine and not on a shared CI runner — it
// timed out on rc.11's macOS and Linux jobs and keeps the sanitizer job red,
// where the instrumented handshake is several times slower again. Generous on
// purpose: a healthy run never waits anywhere near this long, so it only
// affects how quickly a genuinely broken run gives up.
constexpr auto kSetupWait = std::chrono::seconds(45);


constexpr int kPayloadType = 102;
constexpr const char* kMid = "vscreen";

struct Latch {
    std::mutex m;
    std::condition_variable cv;
    bool hit = false;
    std::vector<std::byte> payload;

    void set(std::vector<std::byte> data) {
        {
            std::lock_guard lock(m);
            if (hit) return;
            hit = true;
            payload = std::move(data);
        }
        cv.notify_all();
    }
    bool wait(std::chrono::seconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [this] { return hit; });
    }
};

// Mirror of PeerConnectionManager::attachVideoTrack's chain.
std::shared_ptr<rtc::H264RtpPacketizer> buildChain(
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig) {
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, rtpConfig);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([]() {}));
    packetizer->addToChain(std::make_shared<rtc::PacingHandler>(
        20'000'000.0, std::chrono::milliseconds(5)));
    packetizer->addToChain(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::LongStartSequence));
    packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    return packetizer;
}

} // namespace

// The canary case (initialOffer=false) is EXPECTED to time out, so it does
// not get the full handshake allowance — every run would otherwise pay 45s
// twice over for a failure we already know about. Nothing is at risk in
// keeping it short: main() ignores this case's result, so a slow machine can
// only cost us the NOTE we would print if upstream ever fixed the
// limitation, never a red run.
constexpr auto kCanaryWait = std::chrono::seconds(10);

// initialOffer=true: track present from the first offer (simple case).
// initialOffer=false: PRODUCTION SHAPE — connect with a data channel
// only, then addTrack + renegotiate on the live connection, exactly
// like PeerConnectionManager's "Adding video tracks + renegotiating".
int runCase(bool initialOffer) {
    const auto setupWait = initialOffer ? kSetupWait : kCanaryWait;
    std::printf("--- case: track via %s\n",
                initialOffer ? "initial offer" : "renegotiation");
    rtc::Configuration cfg; // no ICE servers — loopback host candidates
    // Pin ICE to loopback. On GitHub's hosted runners (several interfaces,
    // IPv6, docker bridges) libjuice's default 'any' bind let two in-process
    // peers gather candidates on addresses that never connect to each other,
    // and the handshake waited out its full timeout — on Linux and macOS,
    // on changes unrelated to this test. Locally it always passed.
    cfg.bindAddress = "127.0.0.1";

    auto pcA = std::make_shared<rtc::PeerConnection>(cfg);
    auto pcB = std::make_shared<rtc::PeerConnection>(cfg);

    // Local signaling: pipe descriptions/candidates across directly.
    pcA->onLocalDescription([&](rtc::Description d) {
        pcB->setRemoteDescription(d);
    });
    pcB->onLocalDescription([&](rtc::Description d) {
        pcA->setRemoteDescription(d);
    });
    pcA->onLocalCandidate([&](rtc::Candidate c) { pcB->addRemoteCandidate(c); });
    pcB->onLocalCandidate([&](rtc::Candidate c) { pcA->addRemoteCandidate(c); });

    // Receiver side adopts the track like our onTrack handler does.
    Latch received;
    std::shared_ptr<rtc::Track> recvTrackKeepalive;
    std::mutex recvMutex;
    pcB->onTrack([&](std::shared_ptr<rtc::Track> track) {
        auto recvConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            535353, "bsfchat-test-b", kPayloadType,
            rtc::H264RtpPacketizer::defaultClockRate);
        track->setMediaHandler(buildChain(recvConfig));
        track->onFrame([&](rtc::binary data, rtc::FrameInfo) {
            received.set(std::move(data));
        });
        std::lock_guard lock(recvMutex);
        recvTrackKeepalive = track;
    });

    auto addSendTrack = [&]() {
        rtc::Description::Video media(kMid,
                                      rtc::Description::Direction::SendRecv);
        media.addH264Codec(kPayloadType);
        auto track = pcA->addTrack(std::move(media));
        auto config = std::make_shared<rtc::RtpPacketizationConfig>(
            424242, "bsfchat-test", kPayloadType,
            rtc::H264RtpPacketizer::defaultClockRate);
        track->setMediaHandler(buildChain(config));
        return std::make_pair(track, config);
    };

    std::shared_ptr<rtc::Track> sendTrack;
    std::shared_ptr<rtc::RtpPacketizationConfig> sendConfig;
    std::shared_ptr<rtc::DataChannel> dc;

    Latch dcOpen;
    Latch sendOpen;
    if (initialOffer) {
        // Track BEFORE the data channel: createDataChannel can auto-
        // trigger negotiation immediately, and an offer that races
        // ahead of addTrack carries no media section — the answerer
        // then builds a DTLS transport without SRTP. Mirrors the
        // production ordering in PeerConnectionManager::createOffer.
        std::tie(sendTrack, sendConfig) = addSendTrack();
        sendTrack->onOpen([&]() { sendOpen.set({}); });
        dc = pcA->createDataChannel("kick");
        dc->onOpen([&]() { dcOpen.set({}); });
        pcA->setLocalDescription();
    } else {
        // Phase 1: data-channel-only connection (the legacy shape).
        dc = pcA->createDataChannel("kick");
        dc->onOpen([&]() { dcOpen.set({}); });
        pcA->setLocalDescription();
        if (!dcOpen.wait(setupWait)) {
            std::fprintf(stderr, "FAIL: data channel never opened\n");
            return 2;
        }
        // Phase 2: live renegotiation adds the video m-line — mirrors
        // the pre-fix ensureVideoTracks + triggerRenegotiation flow.
        std::tie(sendTrack, sendConfig) = addSendTrack();
        sendTrack->onOpen([&]() { sendOpen.set({}); });
        pcA->setLocalDescription();
    }

    if (!sendOpen.wait(setupWait)) {
        std::fprintf(stderr, "FAIL: video track never opened over loopback\n");
        return 2;
    }

    // Minimal plausible Annex-B AU: SPS/PPS/IDR NALs with long start
    // codes. Content nonsense is fine — nothing decodes it; it only
    // has to survive packetize → SRTP → depacketize.
    std::vector<std::byte> au;
    auto putNal = [&au](std::initializer_list<int> bytes) {
        const std::byte start[4] = {std::byte{0}, std::byte{0},
                                    std::byte{0}, std::byte{1}};
        au.insert(au.end(), start, start + 4);
        for (int b : bytes) au.push_back(std::byte(b));
    };
    putNal({0x67, 0x42, 0x00, 0x1E, 0x8D, 0x68, 0x05, 0x00, 0x5B, 0xA1});
    putNal({0x68, 0xCE, 0x3C, 0x80});
    putNal({0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0xFE, 0xF6, 0xF0, 0xFE,
            0x05, 0x36, 0x56, 0x04, 0x50, 0x96, 0x7B, 0x3C, 0x50, 0xFF});

    // A few sends spaced out — first packets can race SRTP readiness.
    for (int i = 0; i < 30 && !received.hit; ++i) {
        sendConfig->timestamp = sendConfig->startTimestamp
            + sendConfig->secondsToTimestamp(i / 30.0);
        try {
            sendTrack->send(au.data(), au.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: send threw: %s\n", e.what());
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!received.wait(setupWait)) {
        std::fprintf(stderr,
            "FAIL: no AU ever surfaced from onFrame on the receiving track — "
            "media receive path is broken for this negotiation shape\n");
        return 1;
    }

    std::printf("PASS: AU traversed loopback (%zu bytes sent, %zu received)\n",
                au.size(), received.payload.size());

    pcA->close();
    pcB->close();
    return 0;
}

// ---------------------------------------------------------------------
// S-2: the reliable "control" data channel, and why it is gated on caps
// ---------------------------------------------------------------------
// Keyframe requests used to ride the audio channel, which is unordered
// with maxRetransmits=0 — one lost datagram and the request was gone,
// leaving the viewer frozen until the next periodic IDR. They now ride a
// second, reliable+ordered channel labelled "control".
//
// Opening a second channel is only safe because of two library
// behaviours, and this case proves BOTH rather than assuming them:
//
//   1. GATED PATH (both sides updated): the second channel arrives on
//      the far side as its own onDataChannel with its label intact and
//      with reliable+ordered settings, while the audio channel stays
//      open and unreliable. A receiver that dispatches on the label
//      keeps both.
//   2. LEGACY PATH (peer not updated): a receiver that ignores labels —
//      which is what every build before this one does, its
//      onDataChannel ending in `else setupDataChannel(dc)` — REBINDS
//      its audio channel to whatever arrives second. Its Opus would
//      then ride a reliable, ordered channel and stall behind
//      retransmissions: video fixed, audio damaged.
//
// (2) is the entire reason PeerCaps::controlDc exists and why
// PeerConnectionManager::ensureControlChannel() refuses to open the
// channel toward a peer that has not advertised it. If this assertion
// ever stops holding, that gate can be removed.
int runControlChannelCase() {
    std::printf("--- case: reliable control channel + legacy dispatch\n");
    rtc::Configuration cfg;
    // Pin ICE to loopback. On GitHub's hosted runners (several interfaces,
    // IPv6, docker bridges) libjuice's default 'any' bind let two in-process
    // peers gather candidates on addresses that never connect to each other,
    // and the handshake waited out its full timeout — on Linux and macOS,
    // on changes unrelated to this test. Locally it always passed.
    cfg.bindAddress = "127.0.0.1";

    auto pcA = std::make_shared<rtc::PeerConnection>(cfg);
    auto pcB = std::make_shared<rtc::PeerConnection>(cfg);

    pcA->onLocalDescription([&](rtc::Description d) {
        pcB->setRemoteDescription(d);
    });
    pcB->onLocalDescription([&](rtc::Description d) {
        pcA->setRemoteDescription(d);
    });
    pcA->onLocalCandidate([&](rtc::Candidate c) { pcB->addRemoteCandidate(c); });
    pcB->onLocalCandidate([&](rtc::Candidate c) { pcA->addRemoteCandidate(c); });

    // Receiver side. Two bindings kept in parallel: what an updated
    // build does (dispatch on label) and what an older one does (take
    // whatever arrives as the audio channel).
    std::mutex recvMutex;
    std::shared_ptr<rtc::DataChannel> dispatchAudio;    // updated build
    std::shared_ptr<rtc::DataChannel> dispatchControl;  // updated build
    std::shared_ptr<rtc::DataChannel> legacyAudio;      // pre-update build
    Latch controlMsg;
    Latch audioMsg;

    pcB->onDataChannel([&](std::shared_ptr<rtc::DataChannel> dc) {
        const std::string label = dc->label();
        {
            std::lock_guard lock(recvMutex);
            // The updated dispatch, mirroring PeerConnectionManager.
            if (label == "control") dispatchControl = dc;
            else dispatchAudio = dc;
            // The pre-update dispatch: no label check at all.
            legacyAudio = dc;
        }
        dc->onMessage([&, label](rtc::message_variant msg) {
            if (!std::holds_alternative<rtc::binary>(msg)) return;
            auto& data = std::get<rtc::binary>(msg);
            if (label == "control") controlMsg.set(data);
            else audioMsg.set(data);
        });
    });

    // Offerer: the audio channel exactly as production creates it.
    rtc::DataChannelInit audioInit;
    audioInit.reliability.unordered = true;
    audioInit.reliability.maxRetransmits = 0;
    Latch audioOpen;
    auto audioDc = pcA->createDataChannel("audio", audioInit);
    audioDc->onOpen([&]() { audioOpen.set({}); });
    pcA->setLocalDescription();

    if (!audioOpen.wait(kSetupWait)) {
        std::fprintf(stderr, "FAIL: audio channel never opened\n");
        return 2;
    }

    // In-band second channel, no renegotiation of our own — this is
    // what ensureControlChannel() does once caps allow it.
    Latch controlOpen;
    auto controlDc = pcA->createDataChannel("control");
    controlDc->onOpen([&]() { controlOpen.set({}); });
    if (!controlOpen.wait(kSetupWait)) {
        std::fprintf(stderr, "FAIL: control channel never opened\n");
        return 2;
    }

    // (1) Reliability actually differs — the whole point of the split.
    if (controlDc->reliability().maxRetransmits.has_value()
        || controlDc->reliability().maxPacketLifeTime.has_value()) {
        std::fprintf(stderr, "FAIL: control channel is not reliable\n");
        return 3;
    }
    if (controlDc->reliability().unordered) {
        std::fprintf(stderr, "FAIL: control channel is unordered\n");
        return 3;
    }
    if (!audioDc->reliability().maxRetransmits.has_value()
        || *audioDc->reliability().maxRetransmits != 0) {
        std::fprintf(stderr, "FAIL: audio channel lost maxRetransmits=0\n");
        return 3;
    }

    // (1) Both channels carry their own traffic, addressed by label.
    const std::string kf = "{\"t\":\"kf\",\"stream\":0}";
    controlDc->send(rtc::binary(
        reinterpret_cast<const std::byte*>(kf.data()),
        reinterpret_cast<const std::byte*>(kf.data() + kf.size())));
    const std::string opus = "opus-ish";
    audioDc->send(rtc::binary(
        reinterpret_cast<const std::byte*>(opus.data()),
        reinterpret_cast<const std::byte*>(opus.data() + opus.size())));

    if (!controlMsg.wait(kSetupWait)) {
        std::fprintf(stderr, "FAIL: control message never arrived on the "
                             "control channel\n");
        return 4;
    }
    if (!audioMsg.wait(kSetupWait)) {
        std::fprintf(stderr, "FAIL: audio message never arrived on the "
                             "audio channel\n");
        return 4;
    }

    {
        std::lock_guard lock(recvMutex);
        if (!dispatchAudio || !dispatchControl) {
            std::fprintf(stderr, "FAIL: label dispatch did not see both "
                                 "channels\n");
            return 5;
        }
        if (dispatchAudio->label() != "audio"
            || dispatchControl->label() != "control") {
            std::fprintf(stderr, "FAIL: labels did not survive the wire\n");
            return 5;
        }
        // (2) The hazard the capability gate exists for. A build that
        // ignores labels ends up with its "audio" channel pointing at
        // the control channel.
        if (!legacyAudio || legacyAudio->label() != "control") {
            std::fprintf(stderr, "NOTE: a label-ignoring receiver no longer "
                                 "rebinds to the second channel — the "
                                 "PeerCaps::controlDc gate may be "
                                 "removable\n");
        } else {
            std::printf("expected: a label-ignoring receiver rebinds its "
                        "audio channel to \"control\" — hence the "
                        "controlDc capability gate\n");
        }
    }

    std::printf("PASS: control channel is reliable+ordered, audio stays "
                "unreliable, both deliver\n");
    pcA->close();
    pcB->close();
    return 0;
}

// ---------------------------------------------------------------------
// Late track adoption: onOpen does not replay
// ---------------------------------------------------------------------
// PeerConnectionManager adopts a remote track from onTrack through a
// QUEUED invocation, so attachVideoTrack() runs at least one event-loop
// turn after libdatachannel handed the track over. Meanwhile
// PeerConnection::processRemoteDescription enqueues openTracks() on its
// own processor thread whenever a description arrives while DTLS is
// already connected — so the two race, and the track can be open before
// anyone installs a callback on it.
//
// Channel::onOpen is a plain assignment into the impl (channel.cpp: no
// replay, unlike onMessage which flushes pending messages). This case
// pins that: adopt deliberately late, and assert that the track is
// already open AND that the freshly-installed onOpen never fires.
//
// That combination is why attachVideoTrack() has to consult
// track->isOpen() itself. Without it ctx.open never flips: every access
// unit is dropped by sendVideoFrame, the peer stays pinned to the legacy
// JPEG path, and videoTrackOpen — the IDR request the far side needs
// before it can decode anything — is never emitted.
int runLateAdoptionCase() {
    std::printf("--- case: track adopted after it is already open\n");
    rtc::Configuration cfg;
    // Pin ICE to loopback. On GitHub's hosted runners (several interfaces,
    // IPv6, docker bridges) libjuice's default 'any' bind let two in-process
    // peers gather candidates on addresses that never connect to each other,
    // and the handshake waited out its full timeout — on Linux and macOS,
    // on changes unrelated to this test. Locally it always passed.
    cfg.bindAddress = "127.0.0.1";

    auto pcA = std::make_shared<rtc::PeerConnection>(cfg);
    auto pcB = std::make_shared<rtc::PeerConnection>(cfg);

    pcA->onLocalDescription([&](rtc::Description d) { pcB->setRemoteDescription(d); });
    pcB->onLocalDescription([&](rtc::Description d) { pcA->setRemoteDescription(d); });
    pcA->onLocalCandidate([&](rtc::Candidate c) { pcB->addRemoteCandidate(c); });
    pcB->onLocalCandidate([&](rtc::Candidate c) { pcA->addRemoteCandidate(c); });

    // Hand the track to the "Qt thread" instead of adopting it inline,
    // exactly as onTrack's QMetaObject::invokeMethod does.
    std::mutex handoffMutex;
    std::shared_ptr<rtc::Track> handedOver;
    pcB->onTrack([&](std::shared_ptr<rtc::Track> track) {
        std::lock_guard lock(handoffMutex);
        handedOver = track;
    });

    Latch dcOpen;
    auto dc = pcA->createDataChannel("kick");
    dc->onOpen([&]() { dcOpen.set({}); });

    rtc::Description::Video media(kMid, rtc::Description::Direction::SendRecv);
    media.addH264Codec(kPayloadType);
    auto sendTrack = pcA->addTrack(std::move(media));
    Latch sendOpen;
    sendTrack->onOpen([&]() { sendOpen.set({}); });
    pcA->setLocalDescription();

    if (!sendOpen.wait(kSetupWait)) {
        std::fprintf(stderr, "FAIL: send track never opened\n");
        return 2;
    }

    // Give the receiving side's own openTracks() time to run — this is
    // the delay the queued adoption introduces in production.
    //
    // This was 100 iterations of 50ms: a hard five-second budget on a pure
    // LIVENESS wait, and the one place in this file that could still fail
    // because the machine was busy rather than because the code was wrong.
    // Nothing here defends a latency property — the assertion is about
    // whether onOpen replays, not about how quickly the track arrives — so
    // the wait gets the same generous allowance as every other handshake in
    // this test. A healthy run leaves it in milliseconds; all a longer
    // allowance changes is how quickly a genuinely broken run gives up.
    std::shared_ptr<rtc::Track> recvTrack;
    {
        const auto deadline = std::chrono::steady_clock::now() + kSetupWait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(handoffMutex);
                recvTrack = handedOver;
            }
            if (recvTrack) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!recvTrack) {
        std::fprintf(stderr, "FAIL: onTrack never delivered a track\n");
        return 2;
    }

    // Wait for the track to actually open, rather than sleeping a flat 500ms
    // and giving up. The old flat sleep made a LOADED box quietly skip the
    // assertion below via the NOTE path — the test went green having checked
    // nothing, which is a worse failure than a red one because nobody sees
    // it. Polling to a deadline means load delays the check instead of
    // cancelling it; the NOTE is then reserved for a run that genuinely never
    // reached the racing state.
    {
        const auto deadline = std::chrono::steady_clock::now() + kSetupWait;
        while (!recvTrack->isOpen()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    if (!recvTrack->isOpen()) {
        std::printf("NOTE: receive track not open yet at late adoption — "
                    "this run did not reach the racing state; the "
                    "isOpen() check in attachVideoTrack costs nothing "
                    "and still covers the runs that do\n");
        pcA->close();
        pcB->close();
        return 0;
    }

    // THE ASSERTION: installing onOpen on an already-open track does
    // NOT fire it. Anything relying on that callback alone to learn the
    // track is usable never learns it.
    Latch lateOpen;
    recvTrack->onOpen([&]() { lateOpen.set({}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (lateOpen.hit) {
        std::printf("NOTE: onOpen DID replay on an already-open track — "
                    "upstream behaviour changed; the isOpen() check in "
                    "attachVideoTrack is now belt-and-braces\n");
        pcA->close();
        pcB->close();
        return 0;
    }

    std::printf("PASS: already-open track never fires a late onOpen — "
                "attachVideoTrack must read isOpen() itself (it does)\n");
    pcA->close();
    pcB->close();
    return 0;
}

int main() {
    rtc::InitLogger(rtc::LogLevel::Warning);

    // Production shape since the createOffer fix: media m-lines in the
    // INITIAL offer. This must work.
    const int simple = runCase(/*initialOffer=*/true);

    // Known libdatachannel v0.24.5 limitation (the bug this test was
    // written to catch): a data-channel-only first negotiation builds
    // a DTLS transport without SRTP, and tracks added by renegotiation
    // can never carry media ("connection has no media transport").
    // createOffer works around it by announcing the m-lines up front.
    // This case is a CANARY, expected to fail — if an upstream bump
    // makes it pass, the workaround can likely be retired.
    const int renegotiated = runCase(/*initialOffer=*/false);
    if (renegotiated == 0) {
        std::printf("NOTE: renegotiated-track case now PASSES — upstream "
                    "limitation lifted; the initial-offer workaround in "
                    "PeerConnectionManager::createOffer may be removable.\n");
    } else {
        std::printf("expected: renegotiated-track case still fails upstream "
                    "(workaround in createOffer remains necessary)\n");
    }

    // S-2's transport assumptions, both the gated and the legacy path.
    const int control = runControlChannelCase();

    // Why attachVideoTrack cannot rely on onOpen alone.
    const int lateAdoption = runLateAdoptionCase();

    // Join libdatachannel's global worker threads before static
    // destruction — otherwise the process can segfault at exit (seen
    // under ctest, where the harness reaps fast).
    rtc::Cleanup().wait();

    if (simple != 0) return simple;
    if (control != 0) return control;
    return lateAdoption;
}
