#pragma once

#include <cstdint>

// Turns a peer's cumulative receiver reports into a delivery ratio for
// the rate controller (S-3).
//
// The naive form — (bytes the peer received this window) / (bytes we
// sent this window) — reads BYTES STILL IN FLIGHT AS LOSS. That is not
// a rounding error, it is structural: a report boundary falls wherever
// it falls, and an IDR is 10-20x a P-frame, so a keyframe landing near
// the end of a window makes the window look ~50 % lost. The controller
// then cuts the bitrate 25 % and steps the resolution ladder down, and
// the next window (which now contains the IDR's delivery and no new
// keyframe) looks perfect, so it climbs back. That is the quality pulse
// users describe as "it keeps going blurry and sharp again".
//
// The fix is to lag the denominator by one report: bytes received
// during window k are compared against bytes SENT during window k-1,
// which is the traffic those receipts can actually be receipts for. It
// costs one report (500 ms) of reaction time on genuine loss — cheap
// against a false back-off on every keyframe.
//
// Pure arithmetic, no Qt, so the control loop's input can be unit
// tested against synthetic report sequences.
class DeliveryRatioEstimator {
public:
    // `rxBytes` is the peer's cumulative received-byte count from its
    // latest report; `txBytes` is our cumulative sent-byte count toward
    // that peer right now. Returns true and writes `ratio` (0..1) when
    // there is a window to grade; false for the first report, for an
    // idle window, and after a counter reset.
    bool update(uint64_t rxBytes, uint64_t txBytes, double& ratio) {
        // Counters only ever grow; a smaller value means the peer (or
        // our own track) restarted, so the deltas are meaningless.
        const bool reset = rxBytes < m_rxBytes || txBytes < m_txBytes;
        const uint64_t dRx = reset ? 0 : rxBytes - m_rxBytes;
        const uint64_t dTx = reset ? 0 : txBytes - m_txBytes;
        m_rxBytes = rxBytes;
        m_txBytes = txBytes;

        const uint64_t basis = m_prevDTx;
        const bool haveBasis = m_haveBasis && !reset;
        m_prevDTx = dTx;
        m_haveBasis = !reset;

        if (!haveBasis || basis == 0) return false;   // nothing to grade
        double r = double(dRx) / double(basis);
        if (r > 1.0) r = 1.0;   // early receipts from the current window
        ratio = r;
        return true;
    }

private:
    uint64_t m_rxBytes = 0;
    uint64_t m_txBytes = 0;
    // Bytes sent during the PREVIOUS window — the denominator the next
    // report's receipts belong to.
    uint64_t m_prevDTx = 0;
    bool m_haveBasis = false;
};
