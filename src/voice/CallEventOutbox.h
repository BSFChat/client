#pragma once

// Retry queue for OUTBOUND call signalling — V-M2.
//
// Every m.call.* event was fire-and-forget: `sendRoomEvent` reported
// success or failure through a signal nobody listened to on this path, so
// a single lost invite, answer or hangup cost 30 s — the peer sat in
// Connecting until the setup watchdog reaped it and the mesh reconciler
// tried again on its next poll. On a flaky link that is the difference
// between "voice takes a moment" and "voice is broken".
//
// Signalling is small, rare and idempotent on the receiving side (call ids
// deduplicate), so retrying is safe and cheap. What it must NOT do is
// retry forever: a peer that has genuinely gone stays gone, and the
// watchdog is a better answer than an unbounded queue.
//
// Pure logic — no Qt objects beyond value types, no network, no clock of
// its own (the caller passes `nowMs`). That is what makes the backoff
// schedule testable without waiting real seconds for it.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>

namespace voice {

class CallEventOutbox {
public:
    struct Entry {
        quint64 token = 0;
        QString type;        // m.call.invite, m.call.answer, …
        QByteArray payload;  // serialised content
        int attempt = 0;     // 1 on the first send
    };

    // Total tries per event, including the first. Four tries spread over
    // ~3.5 s, which fits comfortably inside the 30 s setup watchdog that
    // is the backstop.
    static constexpr int kMaxAttempts = 4;
    static constexpr qint64 kBaseBackoffMs = 500;
    static constexpr qint64 kMaxBackoffMs = 4000;

    // Exponential, capped: 500, 1000, 2000 ms before attempts 2, 3, 4.
    static qint64 backoffMs(int attemptsSoFar)
    {
        qint64 delay = kBaseBackoffMs;
        for (int i = 1; i < attemptsSoFar && delay < kMaxBackoffMs; ++i)
            delay *= 2;
        return std::min(delay, kMaxBackoffMs);
    }

    // Queue an event for immediate sending. Returns its token.
    quint64 add(const QString& type, const QByteArray& payload, qint64 nowMs)
    {
        Item item;
        item.entry.token = ++m_nextToken;
        item.entry.type = type;
        item.entry.payload = payload;
        item.dueAtMs = nowMs;
        m_items.append(item);
        return item.entry.token;
    }

    // Everything whose next attempt is due. Each returned entry is marked
    // in flight, so a caller polling on a timer cannot send it twice while
    // waiting for the first reply.
    QVector<Entry> due(qint64 nowMs)
    {
        QVector<Entry> out;
        for (auto& item : m_items) {
            if (item.inFlight || item.dueAtMs > nowMs) continue;
            item.inFlight = true;
            item.entry.attempt += 1;
            out.append(item.entry);
        }
        return out;
    }

    // The server accepted it.
    void onSent(quint64 token)
    {
        m_items.removeIf([token](const Item& i) { return i.entry.token == token; });
    }

    // The send failed. Returns true if it will be retried, false if it has
    // exhausted its attempts and been dropped (the caller should log that
    // — it is the case where a peer genuinely never hears from us).
    bool onFailed(quint64 token, qint64 nowMs)
    {
        for (auto& item : m_items) {
            if (item.entry.token != token) continue;
            item.inFlight = false;
            if (item.entry.attempt >= kMaxAttempts) {
                m_items.removeIf([token](const Item& i) {
                    return i.entry.token == token;
                });
                return false;
            }
            item.dueAtMs = nowMs + backoffMs(item.entry.attempt);
            return true;
        }
        return false;   // unknown token: already sent, or already dropped
    }

    // Drop everything — the session is over and nothing queued is worth
    // sending into a call that no longer exists.
    void clear() { m_items.clear(); }

    int pendingCount() const { return int(m_items.size()); }
    bool isEmpty() const { return m_items.isEmpty(); }

private:
    struct Item {
        Entry entry;
        qint64 dueAtMs = 0;
        bool inFlight = false;
    };
    QVector<Item> m_items;
    quint64 m_nextToken = 0;
};

} // namespace voice
