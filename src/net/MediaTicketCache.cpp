#include "net/MediaTicketCache.h"

#include <QDateTime>
#include <QTimer>
#include <QUrl>

#include "util/MediaUrl.h"

#include <algorithm>

namespace bsfchat::client {

MediaTicketCache::MediaTicketCache(QObject* parent, Clock clock)
    : QObject(parent)
    , m_clock(clock ? std::move(clock)
                    : Clock([] { return QDateTime::currentMSecsSinceEpoch(); }))
{
}

void MediaTicketCache::setHomeserver(const QString& homeserver)
{
    if (m_homeserver == homeserver) return;
    m_homeserver = homeserver;
    // A ticket is signed by one server for one object id. Pointing at a
    // different homeserver makes every cached one meaningless.
    clear();
}

void MediaTicketCache::clear()
{
    m_entries.clear();
}

QString MediaTicketCache::composeUrl(const QString& homeserver, const QString& mxcUri,
                                     const QString& mt, qint64 expUnixSeconds)
{
    if (mt.isEmpty()) return {};
    const QString base = buildMediaDownloadUrl(homeserver, mxcUri);
    if (base.isEmpty()) return {};

    return base
         + QStringLiteral("?mt=") + QString::fromUtf8(QUrl::toPercentEncoding(mt))
         + QStringLiteral("&exp=") + QString::number(expUnixSeconds);
}

QString MediaTicketCache::urlFor(const QString& mxcUri) const
{
    if (m_homeserver.isEmpty() || !mxcUri.startsWith(QStringLiteral("mxc://"))) return {};

    const qint64 now = m_clock();
    auto it = m_entries.find(mxcUri);

    if (it == m_entries.end()) {
        Entry e;
        e.lastUsedMs = now;
        e.inFlight = true;
        m_entries.insert(mxcUri, e);
        evictIfNeeded();
        scheduleMint(mxcUri);
        return {};
    }

    it->lastUsedMs = now;

    const bool live = !it->mt.isEmpty() && now < it->deadlineMs;
    const bool stale = live && (now >= it->deadlineMs - kRefreshMarginMs);

    // Refresh ahead of the cliff, and keep serving the still-valid URL while
    // the replacement is in flight. Nothing on screen changes; the next
    // urlFor() after the reply lands simply gets a fresher signature.
    if (!live || stale) {
        if (!it->inFlight && now >= it->retryAfterMs) {
            it->inFlight = true;
            scheduleMint(mxcUri);
        }
    }

    if (!live) {
        // Hold the dead signature rather than erasing it: an entry that keeps
        // its failure count is an entry that keeps its backoff, and dropping it
        // here is exactly how a refused object turns into a request loop.
        return {};
    }
    return composeUrl(m_homeserver, mxcUri, it->mt, it->exp);
}

void MediaTicketCache::storeTicket(const QString& mxcUri, const QString& mt,
                                   qint64 expUnixSeconds, qint64 ttlMs)
{
    auto& e = m_entries[mxcUri];
    e.mt = mt;
    e.exp = expUnixSeconds;
    // Local clock, from the server's relative TTL — see the header for why this
    // is not derived from `exp`.
    e.deadlineMs = m_clock() + std::max<qint64>(ttlMs, 0);
    e.lastUsedMs = m_clock();
    e.inFlight = false;
    e.failures = 0;
    e.retryAfterMs = 0;
    evictIfNeeded();

    const QString url = composeUrl(m_homeserver, mxcUri, mt, expUnixSeconds);
    if (!url.isEmpty()) emit ticketReady(mxcUri, url);
}

void MediaTicketCache::mintFailed(const QString& mxcUri, const QString& error)
{
    auto& e = m_entries[mxcUri];
    e.inFlight = false;
    e.failures += 1;
    // Exponential, capped. The common cause of a refusal is a permanent one —
    // the object is in a channel this user cannot see — so the sequence has to
    // flatten out rather than keep asking.
    qint64 backoff = kFirstBackoffMs;
    for (int i = 1; i < e.failures && backoff < kMaxBackoffMs; ++i) backoff *= 2;
    e.retryAfterMs = m_clock() + std::min(backoff, kMaxBackoffMs);
    emit ticketFailed(mxcUri, error);
}

bool MediaTicketCache::hasLiveTicket(const QString& mxcUri) const
{
    auto it = m_entries.constFind(mxcUri);
    if (it == m_entries.constEnd()) return false;
    return !it->mt.isEmpty() && m_clock() < it->deadlineMs;
}

void MediaTicketCache::scheduleMint(const QString& mxcUri) const
{
    // Queued, never inline: urlFor() is called during QML binding evaluation
    // and from inside MessageModel's event ingestion, and neither is a place to
    // re-enter the network stack.
    auto* self = const_cast<MediaTicketCache*>(this);
    QTimer::singleShot(0, self, [self, mxcUri] { emit self->mintRequested(mxcUri); });
}

void MediaTicketCache::evictIfNeeded() const
{
    if (m_entries.size() <= kMaxEntries) return;
    // Drop the coldest quarter in one pass rather than one entry per insert —
    // an insert that has to scan the whole table is fine occasionally and not
    // fine every time.
    const int target = kMaxEntries * 3 / 4;
    QList<QPair<qint64, QString>> byAge;
    byAge.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        // Never evict something a reply is still coming for: the reply would
        // then resurrect a half-built entry with no use record.
        if (it->inFlight) continue;
        byAge.append({it->lastUsedMs, it.key()});
    }
    std::sort(byAge.begin(), byAge.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [age, key] : byAge) {
        if (m_entries.size() <= target) break;
        m_entries.remove(key);
    }
}

} // namespace bsfchat::client
