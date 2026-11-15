#pragma once

#include <QByteArray>
#include <QtGlobal>

// Receive-side reassembly of one stream's chunked lossless-video frames
// (the "video-lossless" data channel, flags bit 0x2: header carries
// [seq:u32][chunkIdx:u16][chunkTotal:u16]). The channel is reliable and
// ordered, so the chunks of one frame arrive contiguously; anything else
// drops the partial frame and waits for the next chunk 0.
//
// `total` is peer-supplied: up to 65535 chunks of up to the SCTP message
// size (~256 KB) each, i.e. ~16 GB if believed (V-7). So the bytes kept
// for one frame are capped, and a frame that would exceed the cap is
// dropped as a whole — the rest of its chunks are ignored until the next
// chunk 0. Pure logic; no Qt event loop, no libdatachannel.
class LosslessChunkAssembler {
public:
    // A 4K identity-I444 lossless keyframe is ~25 MB in the worst case
    // (incompressible content ≈ raw 3840x2160x3); 32 MB leaves headroom
    // and still bounds a hostile peer to 32 MB per stream.
    static constexpr qsizetype kMaxFrameBytes = 32 * 1024 * 1024;

    enum class Result {
        Incomplete,  // chunk accepted, more to come
        Complete,    // *out holds the reassembled frame
        Dropped,     // out-of-order chunk or gap: partial frame discarded
        Oversize,    // frame exceeded the byte cap: discarded
        Ignored,     // malformed header, or a chunk of an already-dropped frame
    };

    explicit LosslessChunkAssembler(qsizetype maxFrameBytes = kMaxFrameBytes)
        : m_max(maxFrameBytes) {}

    Result add(quint32 seq, quint16 idx, quint16 total,
               const char* payload, qsizetype len, QByteArray* out) {
        if (total == 0 || idx >= total || len < 0) return Result::Ignored;
        if (idx == 0) {
            m_buf.clear();
            m_seq = seq;
            m_next = 0;
            m_skipping = false;
        } else if (m_skipping && seq == m_seq) {
            return Result::Ignored;  // tail of a frame we already dropped
        } else if (seq != m_seq || idx != m_next || m_skipping) {
            reset();
            return Result::Dropped;
        }
        if (len > m_max - m_buf.size()) {
            reset();
            m_seq = seq;
            m_skipping = true;
            return Result::Oversize;
        }
        m_buf.append(payload, len);
        m_next = quint16(idx + 1);
        if (idx + 1 < total) return Result::Incomplete;

        if (out) *out = std::move(m_buf);
        m_buf = QByteArray();
        m_next = 0;
        return Result::Complete;
    }

    qsizetype bufferedBytes() const { return m_buf.size(); }

private:
    void reset() {
        m_buf = QByteArray();  // release, not just clear(): it may be large
        m_next = 0;
        m_skipping = false;
    }

    qsizetype m_max;
    QByteArray m_buf;
    quint32 m_seq = 0;
    quint16 m_next = 0;
    bool m_skipping = false;
};
