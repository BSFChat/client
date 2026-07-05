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
#include <thread>
#include <vector>

namespace {

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

// initialOffer=true: track present from the first offer (simple case).
// initialOffer=false: PRODUCTION SHAPE — connect with a data channel
// only, then addTrack + renegotiate on the live connection, exactly
// like PeerConnectionManager's "Adding video tracks + renegotiating".
int runCase(bool initialOffer) {
    std::printf("--- case: track via %s\n",
                initialOffer ? "initial offer" : "renegotiation");
    rtc::Configuration cfg; // no ICE servers — loopback host candidates

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

    Latch dcOpen;
    auto dc = pcA->createDataChannel("kick");
    dc->onOpen([&]() { dcOpen.set({}); });

    Latch sendOpen;
    if (initialOffer) {
        std::tie(sendTrack, sendConfig) = addSendTrack();
        sendTrack->onOpen([&]() { sendOpen.set({}); });
        pcA->setLocalDescription();
    } else {
        // Phase 1: data-channel-only connection (production's initial
        // audio negotiation).
        pcA->setLocalDescription();
        if (!dcOpen.wait(std::chrono::seconds(10))) {
            std::fprintf(stderr, "FAIL: data channel never opened\n");
            return 2;
        }
        // Phase 2: live renegotiation adds the video m-line — mirrors
        // ensureVideoTracks + triggerRenegotiation.
        std::tie(sendTrack, sendConfig) = addSendTrack();
        sendTrack->onOpen([&]() { sendOpen.set({}); });
        pcA->setLocalDescription();
    }

    if (!sendOpen.wait(std::chrono::seconds(10))) {
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

    if (!received.wait(std::chrono::seconds(5))) {
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

    return simple;
}
