#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <functional>

namespace bsfchat::client {

// Mint-and-cache layer in front of media download URLs.
//
// WHY THIS EXISTS. QML's Image.source and MediaPlayer.source take a URL and
// nothing else — no headers, no callbacks. So for years the viewer's 90-day
// access token rode in that URL's query string, which put it in nginx's access
// log, in the system browser's address bar and history whenever the file card
// was clicked, and in the clipboard whenever anyone copied a link. That is the
// exfiltration channel of the one-click account takeover (audit A1).
//
// A ticket replaces it: the client POSTs the mxc URI to the server, the server
// re-runs the same permission check the download path runs and answers with a
// short-lived signature scoped to that one object and this one user, and the
// client puts THAT in the URL. Five minutes, one object, one user, no session
// credential. See the server's api/MediaTicket.h for what it commits to.
//
// SYNCHRONOUS READ, ASYNCHRONOUS FILL. urlFor() cannot block: it is called from
// QML binding evaluation and from MessageModel::eventToEntry. It answers with
// whatever it has — a ready URL, or an empty string — and when it has nothing
// it schedules a mint and emits ticketReady() later. Every consumer already
// copes with a media URL that arrives after the thing that shows it: the
// message row's mediaUrl is a role that can change (MessageBubble binds to it),
// and the avatar bindings re-evaluate on ServerConnection::mediaTicketEpoch.
//
// The mint is scheduled through the event loop rather than issued inline. That
// is not incidental: urlFor() runs inside a QML binding, and re-entering the
// network stack from there would let a reply's side effects land in the middle
// of a binding evaluation.
//
// REFRESHING. A ticket is replaced BEFORE it dies, not after: past
// kRefreshMarginMs of its life the cache keeps serving the still-valid URL and
// mints a replacement in the background, so a long-lived Image never blinks and
// a playing video never has its source yanked. Only when a ticket is genuinely
// past its deadline does urlFor() go back to returning empty.
//
// The deadline is computed from the server's `ttl_ms` against the LOCAL clock,
// not from its absolute `exp` against ours. A client whose clock is minutes
// adrift would otherwise treat every fresh ticket as already dead, or hold a
// dead one well past its use. `exp` is still carried verbatim into the URL,
// because it is part of what the server signed and must be echoed exactly.
class MediaTicketCache : public QObject {
    Q_OBJECT
public:
    // Milliseconds since the epoch. Injectable so the refresh and expiry
    // windows can be tested without sleeping through a five-minute TTL.
    using Clock = std::function<qint64()>;

    explicit MediaTicketCache(QObject* parent = nullptr, Clock clock = {});

    // Re-mint this long before a ticket's deadline. Sized against the server's
    // 300s default so a screenful of images refreshes in one quiet pass rather
    // than all at once at the moment they die.
    static constexpr qint64 kRefreshMarginMs = 60 * 1000;

    // Bound on cached tickets. A long scroll through an image-heavy channel
    // would otherwise accumulate one entry per object for the life of the
    // session. Evicts least-recently-used.
    static constexpr int kMaxEntries = 512;

    // Failure backoff. Without it, an object the user is not allowed to see (a
    // 404 from the mint endpoint) would be re-requested on every single binding
    // evaluation, forever — one HTTP request per repaint.
    static constexpr qint64 kFirstBackoffMs = 2 * 1000;
    static constexpr qint64 kMaxBackoffMs = 5 * 60 * 1000;

    void setHomeserver(const QString& homeserver);

    // Drop everything. Called on logout and on any access-token change: a
    // ticket names the user it was minted for, so another session's tickets are
    // worthless and keeping them would only delay the first correct fetch.
    void clear();

    // The download URL for `mxcUri`, or "" when there is not (yet) a usable
    // ticket. Never blocks. Schedules a mint when it answers empty, and also
    // when the ticket it is about to hand back is close to expiring.
    QString urlFor(const QString& mxcUri) const;

    // Fed by MatrixClient from the POST /_matrix/media/v3/ticket reply.
    // `expUnixSeconds` is the server's own value and goes into the URL
    // unchanged — it is inside the signature. `ttlMs` sets the local deadline.
    void storeTicket(const QString& mxcUri, const QString& mt,
                     qint64 expUnixSeconds, qint64 ttlMs);
    void mintFailed(const QString& mxcUri, const QString& error);

    // Builds the download URL. Static and public so the shape can be asserted
    // without a cache or a server.
    static QString composeUrl(const QString& homeserver, const QString& mxcUri,
                              const QString& mt, qint64 expUnixSeconds);

    // Test seam: is there a live (not merely cached) ticket for this object?
    bool hasLiveTicket(const QString& mxcUri) const;

signals:
    // "Please POST /_matrix/media/v3/ticket for this mxc." MatrixClient owns
    // the HTTP; this class owns the policy. Always delivered from the event
    // loop, never from inside urlFor().
    void mintRequested(QString mxcUri);

    // A URL that was empty (or stale) is now good. MessageModel repaints the
    // rows naming this object; ServerConnection bumps mediaTicketEpoch so the
    // avatar bindings re-evaluate.
    void ticketReady(QString mxcUri, QString url);
    void ticketFailed(QString mxcUri, QString error);

private:
    struct Entry {
        QString mt;
        qint64 exp = 0;          // the server's value, echoed into the URL
        qint64 deadlineMs = 0;   // local clock, from ttl_ms
        qint64 lastUsedMs = 0;
        bool inFlight = false;
        int failures = 0;
        qint64 retryAfterMs = 0;
    };

    void scheduleMint(const QString& mxcUri) const;
    void evictIfNeeded() const;

    Clock m_clock;
    QString m_homeserver;
    // Mutable because this is a cache: urlFor() is a logically-const read that
    // records use, marks an entry in flight and (via a queued call) starts a
    // fetch. Every caller of urlFor() is itself const — QML bindings,
    // MessageModel::eventToEntry — and threading a non-const cache through all
    // of them would buy nothing.
    mutable QHash<QString, Entry> m_entries;
};

} // namespace bsfchat::client
