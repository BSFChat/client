#pragma once

#include <QMetaType>
#include <QtGlobal>

// One window of feedback about OUR outgoing stream toward one peer.
//
// The controller keys off `lost/expected` — a packet-loss fraction
// derived from RTP sequence numbers at the receiver. `goodputKbps` is
// kept as a SECONDARY signal only: sustained goodput far below target
// with zero loss means the encoder or the pacer is the bottleneck, not
// the network, and cutting the bitrate would be exactly wrong.
//
// `hasLoss` is false for a peer running a build that predates the
// packet counters. Such a peer must not be read as 100 % loss (it
// would collapse the share for everyone) and must not be read as 0 %
// loss either (it would let the rate climb on no evidence): it simply
// does not govern, and if NOBODY governs the controller holds.
struct VideoDeliveryReport {
    bool hasLoss = false;
    quint64 expected = 0;
    quint64 lost = 0;
    double goodputKbps = -1.0;   // < 0 = unknown

    double lossPct() const {
        if (!hasLoss || expected == 0) return 0.0;
        return 100.0 * double(lost) / double(expected);
    }
    bool governs() const { return hasLoss && expected > 0; }
};

Q_DECLARE_METATYPE(VideoDeliveryReport)
