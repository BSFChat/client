#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

// Per-account on-disk cache backed by SQLite.
//
// Its whole reason for existing is to let the client skip the server's
// *initial* /sync on launch. An initial sync is the most expensive request
// the server serves — it materialises every joined room's full state and
// runs a VIEW_CHANNEL permission check per room — and until this class was
// implemented (it was a bool-flipping stub) the client paid for one on
// every single start.
//
// Two things are cached:
//   * the `next_batch` token of the last sync we finished processing, so
//     the next launch can resume incrementally, and
//   * a snapshot of each joined room's state events plus its unread count,
//     because an incremental sync only carries state that *changed*: with
//     the token alone a resumed client would come up with an empty sidebar.
//
// Everything here is best-effort and must fail soft. Any problem — missing
// directory, unopenable or malformed file, unexpected schema, corrupt row,
// a token belonging to another account — has to degrade to "no cache",
// which puts the caller back on the full initial sync it used to do
// unconditionally. A slow launch is a nuisance; a client that cannot sync
// is broken.
class LocalCache : public QObject {
    Q_OBJECT

public:
    explicit LocalCache(QObject* parent = nullptr);
    ~LocalCache() override;

    // Opens (creating if necessary) the cache for one (user, homeserver)
    // pair. Each pair gets its own database file, so several accounts, or
    // the same account on two servers, never share a token or a snapshot.
    // Returns false if the cache is unusable — the caller must then treat
    // it as absent and run a full initial sync.
    bool open(const QString& userId, const QString& homeserver);
    void close();
    bool isOpen() const { return m_open; }

    // Absolute path of the backing file, or empty when closed.
    QString databasePath() const { return m_path; }

    // ── sync token ───────────────────────────────────────────────────
    void cacheSyncToken(const QString& token);
    QString syncToken() const;
    void clearSyncToken();

    // Shape check for a server sync token: "s" followed by digits. Pure and
    // static so it can be applied before a cache is even open.
    //
    // This check matters more than it looks. The server does not reject a
    // token it cannot parse — SyncEngine::handle_sync catches the failed
    // stoll and quietly falls back to position 0, which yields a *state-less
    // incremental* sync. Hand it a token of the wrong shape and the client
    // comes up believing it resumed while holding no room state at all.
    static bool isValidSyncToken(const QString& token);

    // Wall-clock age of the cached token in ms, or -1 when there is none.
    // Callers refuse to resume from a snapshot old enough that membership
    // and permission drift is likely.
    qint64 syncTokenAgeMs() const;

    // ── room snapshot ────────────────────────────────────────────────
    QStringList cachedRoomIds() const;

    // Record one sync response: merges every joined room's state events
    // (keyed by (roomId, type, stateKey), so an incremental sync's partial
    // state overwrites matching rows and leaves the rest alone), mirrors
    // each room's unread count, and stores `next_batch`. Runs as one
    // transaction so a crash cannot leave a token newer than the state it
    // belongs to.
    void recordSync(const bsfchat::SyncResponse& response);

    // Rebuild a SyncResponse from the snapshot. Feeding this through the
    // normal sync-processing path is how a resumed client populates its
    // room list without asking the server for anything — it reuses the
    // exact ingestion code, so there is no second, drifting copy of it.
    //
    // Contains state events only, never timeline events, so replaying it
    // cannot make the UI raise notifications for history the user has
    // already seen.
    //
    // Returns false when the snapshot is empty or unreadable.
    bool buildHydrationSync(bsfchat::SyncResponse& out) const;

    // Drop every cached row (token included). Used when the cache turns out
    // to be unusable or belongs to somebody else.
    void clearAll();

    // Schema revision written into PRAGMA user_version. Bump to invalidate
    // every existing cache file; a mismatch wipes rather than migrates,
    // which is safe because this is a cache and never the source of truth.
    static constexpr int kSchemaVersion = 1;

private:
    bool openAt(const QString& path);
    bool ensureSchema();
    void setMeta(const QString& key, const QString& value);
    QString meta(const QString& key) const;

    bool m_open = false;
    QString m_connectionName;
    QString m_path;
    QString m_userId;
    QString m_homeserver;
    // RAM mirror of the persisted token so the hot path (one write per
    // sync) doesn't need a read query to notice a no-op.
    QString m_syncToken;
    qint64 m_syncTokenAt = 0;
};
