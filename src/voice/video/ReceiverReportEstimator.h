#pragma once

#include "voice/video/VideoDeliveryReport.h"

#include <QtGlobal>

// Turns a peer's CUMULATIVE receiver reports into one graded window
// for the rate controller (S-17). Successor to DeliveryRatioEstimator,
// which graded delivered bytes against sent bytes and could not be
// made to read 1.0 on a lossless LAN — see VideoRateController.h.
//
// Two rules that exist because of specific field failures:
//
//  * SEED, DON'T GRADE. The first report is a baseline, never a
//    verdict. A peer's very first rr is routinely written before its
//    first packet has arrived (track open, waiting on the opening
//    IDR), so differencing against it yields "everything was lost".
//    In the field that read as ratio=0.000 three reports running and
//    cut 2338 → 985 kbps in 1.5 s, before a single packet had been
//    graded. So: report 1 seeds, report 2 establishes the basis, and
//    only from report 3 does the controller hear anything.
//
//  * A COUNTER THAT GOES BACKWARDS IS A RESTART, NOT A CATASTROPHE.
//    The peer rebuilt its receive pipeline; the deltas are garbage.
//    Re-seed silently and start the two-report warm-up again.
//
// Pure arithmetic, no Qt objects, so the sender's whole input path can
// be driven from a unit test with synthetic report sequences.
class ReceiverReportEstimator {
public:
    // `rxBytes`/`expected`/`lost` are the peer's cumulative counters;
    // `hasLossFields` is false when the peer's build didn't send them.
    // Returns a report whose `expected` is 0 (and `governs()` false)
    // while there is nothing to grade.
    VideoDeliveryReport update(quint64 rxBytes, quint64 expected,
                               quint64 lost, bool hasLossFields,
                               qint64 nowMs) {
        VideoDeliveryReport out;
        out.hasLoss = hasLossFields;

        const bool backwards = rxBytes < m_rxBytes || expected < m_expected
            || lost < m_lost;
        if (backwards || !m_seeded) {
            seed(rxBytes, expected, lost, nowMs);
            return out;                      // baseline only
        }

        const quint64 dRx = rxBytes - m_rxBytes;
        const quint64 dExp = expected - m_expected;
        const quint64 dLost = lost - m_lost;
        const qint64 dtMs = nowMs - m_atMs;
        m_rxBytes = rxBytes;
        m_expected = expected;
        m_lost = lost;
        m_atMs = nowMs;

        // Warm-up: the first difference after a seed is discarded, for
        // the reason in the header comment.
        if (!m_warm) {
            m_warm = true;
            return out;
        }

        if (dtMs > 0 && dRx > 0)
            out.goodputKbps = double(dRx) * 8.0 / double(dtMs);
        if (hasLossFields && dExp > 0) {
            out.expected = dExp;
            out.lost = dLost;
        }
        return out;
    }

    void reset() { *this = ReceiverReportEstimator(); }

private:
    void seed(quint64 rxBytes, quint64 expected, quint64 lost, qint64 nowMs) {
        m_rxBytes = rxBytes;
        m_expected = expected;
        m_lost = lost;
        m_atMs = nowMs;
        m_seeded = true;
        m_warm = false;
    }

    quint64 m_rxBytes = 0;
    quint64 m_expected = 0;
    quint64 m_lost = 0;
    qint64 m_atMs = 0;
    bool m_seeded = false;
    bool m_warm = false;
};
