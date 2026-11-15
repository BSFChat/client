// The bundled libdatachannel's video depacketizer, as patched by
// cmake/patch-libdatachannel.cmake (V-6). Unpatched, one malformed STAP-A
// packet made reassemble() throw out of incoming() before the buffer was
// cleared, so every later frame on the track failed with it and the
// buffer grew without bound.
//
// No network: packets are fed straight into rtc::H264RtpDepacketizer.

#include <QtTest>

#include <rtc/rtc.hpp>

#include <cstdint>
#include <vector>

namespace {

rtc::message_ptr rtpPacket(uint16_t seq, uint32_t ts, bool marker,
                           const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> b = {
        0x80, uint8_t((marker ? 0x80 : 0x00) | 96),
        uint8_t(seq >> 8), uint8_t(seq),
        uint8_t(ts >> 24), uint8_t(ts >> 16), uint8_t(ts >> 8), uint8_t(ts),
        0x12, 0x34, 0x56, 0x78,
    };
    b.insert(b.end(), payload.begin(), payload.end());
    rtc::binary bin(b.size());
    for (size_t i = 0; i < b.size(); ++i) bin[i] = std::byte(b[i]);
    return rtc::make_message(std::move(bin));
}

// Runs one batch through the depacketizer; returns the frames it emitted.
rtc::message_vector feed(rtc::H264RtpDepacketizer& d, rtc::message_vector in)
{
    // incoming() is private on the depacketizer, public on MediaHandler.
    static_cast<rtc::MediaHandler&>(d).incoming(in, [](rtc::message_ptr) {});
    return in;
}

} // namespace

class TestRtpDepacketizer : public QObject {
    Q_OBJECT
private slots:
    void malformedStapADoesNotStallTrack() {
        rtc::H264RtpDepacketizer d(rtc::NalUnit::Separator::LongStartSequence);
        // STAP-A (type 24) claiming a 0xFFFF-byte NAL in a 3-byte payload.
        rtc::message_vector out;
        try {
            out = feed(d, {rtpPacket(1, 1000, true, {0x18, 0xFF, 0xFF, 0x01})});
        } catch (const std::exception& e) {
            QFAIL(qPrintable(QStringLiteral("incoming() threw: %1").arg(e.what())));
        }
        QVERIFY(out.empty());

        // The next frame (single NAL, IDR) must come through on its own.
        out = feed(d, {rtpPacket(2, 4000, true, {0x65, 0xAA, 0xBB})});
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0]->size(), size_t(4 + 3));  // start code + NAL
    }

    void nalTypeZeroDropsOnlyThatFrame() {
        rtc::H264RtpDepacketizer d(rtc::NalUnit::Separator::LongStartSequence);
        auto out = feed(d, {rtpPacket(10, 1000, true, {0x00, 0x01})});
        QVERIFY(out.empty());
        out = feed(d, {rtpPacket(11, 2000, true, {0x41, 0x01})});
        QCOMPARE(out.size(), size_t(1));
    }

    void bufferIsBoundedWithoutMarker() {
        // 1000 packets of one timestamp and no marker bit: unpatched they
        // all accumulate; patched the buffer is flushed before its sequence
        // span reaches 512, so the frame finally emitted holds fewer.
        rtc::H264RtpDepacketizer d(rtc::NalUnit::Separator::LongStartSequence);
        rtc::message_vector batch;
        for (uint16_t s = 0; s < 1000; ++s)
            batch.push_back(rtpPacket(s, 5000, false, {0x41, 0x01}));
        batch.push_back(rtpPacket(1000, 5000, true, {0x41, 0x01}));
        const auto out = feed(d, std::move(batch));
        QCOMPARE(out.size(), size_t(1));
        QVERIFY(out[0]->size() < size_t(512 * 6));  // 6 bytes per NAL
    }

    void sequenceJumpWithinFrameFlushes() {
        // Two packets of one timestamp half the sequence space apart
        // would break the int16 comparator's ordering; the second must
        // start a fresh buffer instead.
        rtc::H264RtpDepacketizer d(rtc::NalUnit::Separator::LongStartSequence);
        const auto out = feed(d, {rtpPacket(0, 7000, false, {0x41, 0x01}),
                                  rtpPacket(20000, 7000, false, {0x41, 0x02}),
                                  rtpPacket(40000, 7000, true, {0x41, 0x03})});
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0]->size(), size_t(6));
    }
};

QTEST_APPLESS_MAIN(TestRtpDepacketizer)
#include "test_rtp_depacketizer.moc"
