#pragma once

// When a GET /voip/turnServer answer expires, and when to refetch it.
//
// Pure functions over the reply and a wall-clock "now" (ms since the
// epoch), so VoiceSession's refresh scheduling and VoiceEngine's "is this
// credential still good for a NEW peer" check agree, and so both can be
// tested without waiting an hour.
//
// Why wall clock: the old refresh was a single-shot QTimer armed for
// ttl/2. QTimer runs on a monotonic clock that, on macOS and Windows,
// does not advance while the machine sleeps — so a laptop that slept
// through the half-life woke up with expired credentials and a timer
// that still thought it had half an hour to go. The schedule is now an
// absolute wall-clock instant, checked by a short periodic tick, so the
// first tick after wake sees it is overdue.
//
// The expiry instant is `ttl` counted from when the reply arrived. The
// TURN REST username's "<unix-expiry>:<user>" timestamp is what coturn
// enforces, but it is on the SERVER's clock: compared with ours it made a
// machine whose clock is a few minutes off refresh (and rebuild) at the
// wrong time, or believe valid credentials expired (audit 3). The server
// computes that timestamp as its now + ttl, so ttl from receipt is the
// same instant without the skew. The username is used only when a server
// sends no ttl at all.

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace voice::turn {

// The server's documented default (voice.turn_ttl), used when `ttl` is
// absent or nonsensical.
constexpr qint64 kDefaultTtlSeconds = 24 * 3600;
// Refresh this far ahead of expiry at the latest, and never later than
// half-way through the lifetime. A short ttl therefore refreshes at its
// half-life (as before); a 24 h one refreshes 10 minutes early rather
// than 12 h early.
constexpr qint64 kRefreshLeadMs = 10 * 60 * 1000;
// Floor on the interval between refreshes, so a server answering ttl=0
// cannot make us hammer it.
constexpr qint64 kMinRefreshIntervalMs = 1000;
// A credential within this margin of expiry is treated as expired for a
// NEW peer connection: TURN allocation plus ICE can take that long, and
// an allocation refused mid-setup costs the 30 s watchdog.
constexpr qint64 kExpirySafetyMs = 60 * 1000;
// Wall-clock tick that checks the schedule. Bounded so a wake from sleep
// is noticed within this long.
constexpr int kCheckIntervalMs = 30 * 1000;

// Rebuild a relayed peer for fresh credentials only once ITS credentials
// are this close to expiry. Above kRefreshLeadMs, so the scheduled refresh
// always rebuilds; a wake or network-change refresh with hours left does
// not churn every peer (audit 3).
constexpr qint64 kRebuildLeadMs = 15 * 60 * 1000;

inline qint64 ttlSeconds(const QJsonObject& cfg)
{
    const qint64 ttl = qint64(cfg.value(QStringLiteral("ttl")).toDouble(0));
    return ttl > 0 ? ttl : kDefaultTtlSeconds;
}

inline bool hasTtl(const QJsonObject& cfg)
{
    return cfg.value(QStringLiteral("ttl")).toDouble(0) > 0;
}

// Absolute expiry, ms since the epoch. `receivedAtMs` is when the reply
// arrived. 0 for a config with no relay credentials at all (nothing to
// expire).
inline qint64 expiresAtMs(const QJsonObject& cfg, qint64 receivedAtMs)
{
    const QString user = cfg.value(QStringLiteral("username")).toString();
    if (user.isEmpty() && !cfg.contains(QStringLiteral("uris"))) return 0;
    if (hasTtl(cfg)) return receivedAtMs + ttlSeconds(cfg) * 1000;
    const int colon = user.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const qint64 unixSec = user.left(colon).toLongLong(&ok);
        // Sanity: a plausible epoch second (2001..2286), else ignore.
        if (ok && unixSec > 1'000'000'000LL && unixSec < 10'000'000'000LL)
            return unixSec * 1000;
    }
    return receivedAtMs + ttlSeconds(cfg) * 1000;
}

// When to refetch, ms since the epoch.
inline qint64 refreshAtMs(const QJsonObject& cfg, qint64 receivedAtMs)
{
    const qint64 expiry = expiresAtMs(cfg, receivedAtMs);
    if (expiry <= 0) return receivedAtMs + ttlSeconds(cfg) * 500;
    const qint64 lifetime = qMax<qint64>(0, expiry - receivedAtMs);
    const qint64 lead = qMin(kRefreshLeadMs, lifetime / 2);
    return qMax(receivedAtMs + kMinRefreshIntervalMs, expiry - lead);
}

// True when a peer connection built NOW from these credentials would
// risk an allocation refused for expiry.
inline bool unusableForNewPeer(qint64 expiresAt, qint64 nowMs)
{
    return expiresAt > 0 && nowMs + kExpirySafetyMs >= expiresAt;
}

// True when a relayed peer built with credentials expiring at `expiresAt`
// should be rebuilt now that fresher ones exist. Unknown expiry (0): yes.
inline bool dueForRebuild(qint64 expiresAt, qint64 nowMs)
{
    return expiresAt <= 0 || nowMs + kRebuildLeadMs >= expiresAt;
}

} // namespace voice::turn
