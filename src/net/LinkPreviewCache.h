#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <list>

#include "net/MatrixFailure.h"

namespace bsfchat::client {

// Per-connection cache in front of GET /_matrix/media/v3/preview_url (audit
// Q-S4).
//
// WHY THE SERVER FETCHES. Link previews used to be fetched by every viewer's
// own app with a QML XMLHttpRequest: the linked site saw each reader's IP, a
// link to http://192.168.1.1/ made every reader's machine GET their own
// router, the whole response body was buffered, and og:image was handed to an
// Image with no limit. Now the homeserver fetches once, answers a small JSON
// object, and stores og:image as server media (an mxc:// URI) that the client
// loads through the same ticketed download path as any other image. Nothing
// in the client contacts the linked site — and there is deliberately NO
// client-side fallback when the server cannot preview: a server that does not
// implement the endpoint simply means no previews.
//
// WHAT THIS CLASS OWNS is policy only; MatrixClient owns the HTTP:
//   - an LRU of outcomes (kMaxEntries), positive and negative, each with its
//     own TTL, so a dead link is not re-asked on every repaint;
//   - de-duplication: one request per URL however many cards show it;
//   - a concurrency cap and a bounded queue, so scrolling a link-heavy channel
//     does not fire fifty requests at once (and get 429'd for it);
//   - the per-server "this server has no preview endpoint" verdict, remembered
//     for the life of the connection;
//   - the user's "Show link previews" switch: when off, request() never
//     produces a fetch.
//
// Driven by request() from QML (via ServerConnection::requestLinkPreview);
// results arrive through fetchSucceeded()/fetchFailed() and are announced with
// resolved(url). Callers read the outcome with state()/preview().
class LinkPreviewCache : public QObject {
    Q_OBJECT
public:
    // Milliseconds; injectable so TTLs can be tested without sleeping.
    using Clock = std::function<qint64()>;

    explicit LinkPreviewCache(QObject* parent = nullptr, Clock clock = {});

    enum class State {
        Unknown,   // never asked, expired, or dropped from the queue
        Pending,   // queued or in flight; resolved() will follow
        Ready,     // preview() holds something to render
        Empty,     // nothing to show: negative result, unsupported, or disabled
    };

    struct Preview {
        QString title;
        QString description;
        QString siteName;
        QString imageMxc;   // validated mxc://server/id, or empty
        int imageWidth = 0;
        int imageHeight = 0;
    };

    static constexpr int kMaxEntries = 500;
    // Servers cache a preview for about an hour; asking sooner buys nothing.
    static constexpr qint64 kPositiveTtlMs = 60 * 60 * 1000;
    // "Nothing to preview" / forbidden URL. Long enough that a channel of dead
    // links stays quiet, short enough that a page fixed later shows up.
    static constexpr qint64 kNegativeTtlMs = 10 * 60 * 1000;
    // Transport failure, timeout, 5xx: the server may be back in a moment.
    static constexpr qint64 kTransientTtlMs = 60 * 1000;
    static constexpr int kMaxInFlight = 4;
    // Queued-but-not-started requests. Past this the OLDEST is dropped (its
    // card has most likely scrolled away) and resolved() as Unknown.
    static constexpr int kMaxQueued = 64;
    // Text is trimmed here, once, rather than trusting the server (or the page
    // behind it) to send a sensible amount. QML elides further on top.
    static constexpr int kMaxTitleChars = 300;
    static constexpr int kMaxDescriptionChars = 600;
    static constexpr int kMaxSiteNameChars = 100;
    // An og:image the server says is bigger than this is not shown at all.
    static constexpr qint64 kMaxImageBytes = 8 * 1024 * 1024;
    static constexpr int kMaxUrlLength = 2048;

    // "Show link previews". Turning it off drops the queue (resolving each
    // queued URL as Empty) and makes request() a no-op; in-flight replies
    // still land in the cache, which is harmless.
    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }

    // False once the server has told us it has no preview endpoint. Never
    // flips back within a session; a new connection starts optimistic.
    bool supported() const { return m_supported; }

    State state(const QString& url) const;
    // Meaningful only when state(url) == Ready; default-constructed otherwise.
    Preview preview(const QString& url) const;

    // Ask for a preview of `url`. Returns true when resolved(url) will follow
    // (a fetch is queued or already running), false when the answer is
    // already known — read state() — or no fetch will ever be made (disabled,
    // unsupported server, not an http(s) URL).
    bool request(const QString& url);

    // Fed by ServerConnection from MatrixClient's reply.
    void fetchSucceeded(const QString& url, const QJsonObject& body);
    void fetchFailed(const QString& url, const bsfchat::net::MatrixFailure& failure);

    // Pure helpers, public for tests.
    static bool isPreviewableUrl(const QString& url);
    // Does this failure mean "this server has no preview endpoint"?
    static bool isUnsupportedFailure(const bsfchat::net::MatrixFailure& failure);
    // Server JSON -> trimmed, validated Preview. `ok` is false when nothing in
    // it is worth a card.
    static Preview parse(const QJsonObject& body, bool* ok = nullptr);

    // Test seams.
    int size() const { return int(m_entries.size()); }
    int inFlightCount() const { return int(m_inFlight.size()); }
    int queuedCount() const { return int(m_queue.size()); }

signals:
    // "GET preview_url for this URL." Emitted at most kMaxInFlight at a time,
    // and never twice for the same URL while the first is outstanding.
    void fetchRequested(QString url);
    // The outcome for `url` changed; re-read state()/preview().
    void resolved(QString url);
    void supportedChanged();

private:
    struct Entry {
        State state = State::Empty;
        Preview preview;
        qint64 expiresAtMs = 0;
        std::list<QString>::iterator lru;
    };

    qint64 now() const;
    void store(const QString& url, State state, const Preview& preview, qint64 ttlMs);
    void pump();
    void markUnsupported();

    Clock m_clock;
    bool m_enabled = true;
    bool m_supported = true;
    // Most recently used at the front. Mutable: state()/preview() are
    // logically-const reads that refresh recency, called from QML bindings.
    mutable std::list<QString> m_lru;
    mutable QHash<QString, Entry> m_entries;
    QSet<QString> m_inFlight;
    QStringList m_queue;
    // A 429 pauses the queue until then (local clock). 0 = not paused.
    qint64 m_pausedUntilMs = 0;
    bool m_resumeScheduled = false;
};

} // namespace bsfchat::client
