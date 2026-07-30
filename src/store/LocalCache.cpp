#include "store/LocalCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

#include <nlohmann/json.hpp>

namespace {

// Meta keys. `owner_*` exist so a file that somehow ends up describing a
// different account (hash collision, a hand-copied profile directory) is
// wiped instead of silently resumed against the wrong server.
constexpr auto kMetaOwnerUser = "owner_user_id";
constexpr auto kMetaOwnerHost = "owner_homeserver";
constexpr auto kMetaSyncToken = "sync_token";
constexpr auto kMetaSyncTokenAt = "sync_token_at";

// One database file per (user, homeserver). The host prefix keeps the
// directory browsable; the digest carries the actual identity, including the
// user id, which is not filesystem-safe on its own (`@josh:host` has a colon,
// which Windows rejects outright).
QString cacheFileName(const QString& userId, const QString& homeserver)
{
    QString host = QUrl(homeserver).host();
    if (host.isEmpty()) host = homeserver;
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9._-]"));
    host.replace(unsafe, QStringLiteral("_"));
    host.truncate(48);

    const QByteArray digest = QCryptographicHash::hash(
        (homeserver + QChar(0x1f) + userId).toUtf8(),
        QCryptographicHash::Sha256).toHex().left(16);

    return host + QLatin1Char('-') + QString::fromLatin1(digest)
         + QStringLiteral(".sqlite");
}

} // namespace

LocalCache::LocalCache(QObject* parent)
    : QObject(parent)
{
    // Connection names are global to QSqlDatabase, so derive one per
    // instance: several ServerConnections (one per server) each own a cache
    // and would otherwise collide on a shared name.
    m_connectionName = QStringLiteral("bsfchat-localcache-0x%1")
                           .arg(reinterpret_cast<quintptr>(this), 0, 16);
}

LocalCache::~LocalCache()
{
    close();
}

bool LocalCache::isValidSyncToken(const QString& token)
{
    if (token.size() < 2 || token.at(0) != QLatin1Char('s')) return false;
    // Reject anything but "s" + decimal digits, and cap the length so a
    // pathological value can't be handed to the server at all.
    if (token.size() > 24) return false;
    for (int i = 1; i < token.size(); ++i) {
        if (!token.at(i).isDigit()) return false;
    }
    return true;
}

bool LocalCache::open(const QString& userId, const QString& homeserver)
{
    close();
    if (userId.isEmpty() || homeserver.isEmpty()) return false;

    const QString dir = QStandardPaths::writableLocation(
                            QStandardPaths::AppDataLocation)
                      + QStringLiteral("/cache");
    if (dir.isEmpty()) return false;
    if (!QDir().mkpath(dir)) {
        qWarning() << "LocalCache: cannot create" << dir << "— running without a cache";
        return false;
    }

    m_userId = userId;
    m_homeserver = homeserver;
    const QString path = dir + QLatin1Char('/') + cacheFileName(userId, homeserver);

    if (openAt(path)) return true;

    // A file we cannot open or whose schema we cannot establish is almost
    // always a truncated or malformed SQLite image (killed mid-write, full
    // disk, synced by a file-sharing tool). It holds nothing we can't
    // rebuild, so delete it and try once more before giving up.
    qWarning() << "LocalCache: unusable cache at" << path << "— recreating";
    close();
    QFile::remove(path);
    if (openAt(path)) return true;

    qWarning() << "LocalCache: cache unavailable — falling back to full sync";
    close();
    return false;
}

bool LocalCache::openAt(const QString& path)
{
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
        qWarning() << "LocalCache: open failed:" << db.lastError().text();
        return false;
    }
    m_path = path;
    if (!ensureSchema()) return false;

    m_open = true;

    // Guard against a file that describes a different identity. Cheap, and
    // the failure it prevents (resuming another account's token) is one of
    // the nastiest available.
    const QString owner = meta(QString::fromLatin1(kMetaOwnerUser));
    const QString host = meta(QString::fromLatin1(kMetaOwnerHost));
    if ((!owner.isEmpty() && owner != m_userId)
        || (!host.isEmpty() && host != m_homeserver)) {
        qWarning() << "LocalCache: cache belongs to" << owner << "@" << host
                   << "— discarding";
        clearAll();
    }
    setMeta(QString::fromLatin1(kMetaOwnerUser), m_userId);
    setMeta(QString::fromLatin1(kMetaOwnerHost), m_homeserver);

    m_syncToken = meta(QString::fromLatin1(kMetaSyncToken));
    m_syncTokenAt = meta(QString::fromLatin1(kMetaSyncTokenAt)).toLongLong();
    // A token we can't hand back safely is worse than none at all: the
    // server would answer position-0 incremental and the client would come
    // up with no room state. Drop it here, once, rather than making every
    // caller remember to check.
    if (!m_syncToken.isEmpty() && !isValidSyncToken(m_syncToken)) {
        qWarning() << "LocalCache: discarding malformed sync token";
        clearSyncToken();
    }
    return true;
}

bool LocalCache::ensureSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);

    if (!q.exec(QStringLiteral("PRAGMA user_version"))) {
        qWarning() << "LocalCache: user_version read failed:" << q.lastError().text();
        return false;
    }
    const int version = q.next() ? q.value(0).toInt() : -1;

    if (version != kSchemaVersion) {
        // Wipe rather than migrate. This is a cache: the server is always
        // able to rebuild it, and a migration bug here would corrupt the
        // one thing the resume path trusts.
        for (const auto& stmt : {
                 QStringLiteral("DROP TABLE IF EXISTS room_state"),
                 QStringLiteral("DROP TABLE IF EXISTS rooms"),
                 QStringLiteral("DROP TABLE IF EXISTS meta") }) {
            if (!q.exec(stmt)) {
                qWarning() << "LocalCache: schema reset failed:" << q.lastError().text();
                return false;
            }
        }
    }

    for (const auto& stmt : {
             QStringLiteral("CREATE TABLE IF NOT EXISTS meta ("
                            "  key TEXT PRIMARY KEY,"
                            "  value TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS rooms ("
                            "  room_id TEXT PRIMARY KEY,"
                            "  unread_count INTEGER NOT NULL DEFAULT 0)"),
             // state_key is NOT NULL with "" for the common no-key case:
             // SQLite treats NULLs as distinct in a primary key, so a
             // nullable column would let duplicate rows accumulate for
             // every m.room.name update instead of replacing.
             QStringLiteral("CREATE TABLE IF NOT EXISTS room_state ("
                            "  room_id TEXT NOT NULL,"
                            "  type TEXT NOT NULL,"
                            "  state_key TEXT NOT NULL DEFAULT '',"
                            "  json TEXT NOT NULL,"
                            "  PRIMARY KEY (room_id, type, state_key))") }) {
        if (!q.exec(stmt)) {
            qWarning() << "LocalCache: schema create failed:" << q.lastError().text();
            return false;
        }
    }

    if (version != kSchemaVersion
        && !q.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion))) {
        qWarning() << "LocalCache: user_version write failed:" << q.lastError().text();
        return false;
    }
    return true;
}

void LocalCache::close()
{
    m_open = false;
    m_path.clear();
    m_syncToken.clear();
    m_syncTokenAt = 0;
    if (QSqlDatabase::contains(m_connectionName)) {
        // The QSqlDatabase copy must go out of scope before
        // removeDatabase(), or Qt warns and leaks the connection.
        {
            auto db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

void LocalCache::setMeta(const QString& key, const QString& value)
{
    if (!m_open) return;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO meta (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    if (!q.exec()) qWarning() << "LocalCache: meta write failed:" << q.lastError().text();
}

QString LocalCache::meta(const QString& key) const
{
    if (!m_open) return {};
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next()) return {};
    return q.value(0).toString();
}

void LocalCache::cacheSyncToken(const QString& token)
{
    if (!m_open) return;
    if (!isValidSyncToken(token)) {
        // Never persist something we would refuse to read back.
        if (!token.isEmpty())
            qWarning() << "LocalCache: refusing to cache malformed sync token";
        return;
    }
    if (token == m_syncToken) return;
    m_syncToken = token;
    m_syncTokenAt = QDateTime::currentMSecsSinceEpoch();
    setMeta(QString::fromLatin1(kMetaSyncToken), m_syncToken);
    setMeta(QString::fromLatin1(kMetaSyncTokenAt), QString::number(m_syncTokenAt));
}

QString LocalCache::syncToken() const
{
    return m_syncToken;
}

qint64 LocalCache::syncTokenAgeMs() const
{
    if (m_syncToken.isEmpty() || m_syncTokenAt <= 0) return -1;
    return qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_syncTokenAt);
}

void LocalCache::clearSyncToken()
{
    m_syncToken.clear();
    m_syncTokenAt = 0;
    if (!m_open) return;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("DELETE FROM meta WHERE key IN (?, ?)"));
    q.addBindValue(QString::fromLatin1(kMetaSyncToken));
    q.addBindValue(QString::fromLatin1(kMetaSyncTokenAt));
    q.exec();
}

void LocalCache::clearAll()
{
    m_syncToken.clear();
    m_syncTokenAt = 0;
    if (!m_open) return;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    for (const auto& stmt : { QStringLiteral("DELETE FROM room_state"),
                              QStringLiteral("DELETE FROM rooms"),
                              QStringLiteral("DELETE FROM meta") }) {
        if (!q.exec(stmt)) qWarning() << "LocalCache: clear failed:" << q.lastError().text();
    }
}

void LocalCache::recordSync(const bsfchat::SyncResponse& response)
{
    if (!m_open) return;

    const QString nextBatch = QString::fromStdString(response.next_batch);
    // Nothing to do at all — don't open a transaction for it.
    if (response.rooms.join.empty() && nextBatch == m_syncToken) return;

    auto db = QSqlDatabase::database(m_connectionName, false);
    const bool inTransaction = db.transaction();

    QSqlQuery state(db);
    state.prepare(QStringLiteral(
        "INSERT INTO room_state (room_id, type, state_key, json) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(room_id, type, state_key) DO UPDATE SET json = excluded.json"));
    QSqlQuery room(db);
    room.prepare(QStringLiteral(
        "INSERT INTO rooms (room_id, unread_count) VALUES (?, ?) "
        "ON CONFLICT(room_id) DO UPDATE SET unread_count = excluded.unread_count"));
    // Absent unread_count means "unchanged", not zero — an incremental sync
    // omits it for rooms it doesn't mention, and overwriting with 0 would
    // silently clear badges the user hasn't looked at yet.
    QSqlQuery roomSeen(db);
    roomSeen.prepare(QStringLiteral(
        "INSERT INTO rooms (room_id, unread_count) VALUES (?, 0) "
        "ON CONFLICT(room_id) DO NOTHING"));

    bool ok = true;
    for (const auto& [roomIdStr, joined] : response.rooms.join) {
        const QString roomId = QString::fromStdString(roomIdStr);
        if (roomId.isEmpty()) continue;

        if (joined.unread_count.has_value()) {
            room.bindValue(0, roomId);
            room.bindValue(1, *joined.unread_count);
            if (!room.exec()) { ok = false; break; }
        } else {
            roomSeen.bindValue(0, roomId);
            if (!roomSeen.exec()) { ok = false; break; }
        }

        // Only state events are snapshotted. Timeline events are the
        // server's job to replay from the token, and caching them would
        // make the hydration replay indistinguishable from live traffic.
        for (const auto& ev : joined.state.events) {
            if (!ev.state_key.has_value()) continue;
            nlohmann::json j;
            try {
                bsfchat::to_json(j, ev);
            } catch (const std::exception& e) {
                qWarning() << "LocalCache: skipping unserialisable state event:" << e.what();
                continue;
            }
            state.bindValue(0, roomId);
            state.bindValue(1, QString::fromStdString(ev.type));
            state.bindValue(2, QString::fromStdString(*ev.state_key));
            state.bindValue(3, QString::fromStdString(j.dump()));
            if (!state.exec()) { ok = false; break; }
        }
        if (!ok) break;
    }

    if (!ok) {
        qWarning() << "LocalCache: snapshot write failed:" << state.lastError().text();
        if (inTransaction) db.rollback();
        // Leave the token alone. A token newer than the state it belongs to
        // is the one combination that produces a wrong resume, so on a
        // partial write we keep the older, consistent pair.
        return;
    }

    // Token last, inside the same transaction, for exactly that reason.
    if (isValidSyncToken(nextBatch) && nextBatch != m_syncToken) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QSqlQuery m(db);
        m.prepare(QStringLiteral(
            "INSERT INTO meta (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        for (const auto& kv : { qMakePair(QString::fromLatin1(kMetaSyncToken), nextBatch),
                                qMakePair(QString::fromLatin1(kMetaSyncTokenAt),
                                          QString::number(now)) }) {
            m.bindValue(0, kv.first);
            m.bindValue(1, kv.second);
            if (!m.exec()) {
                qWarning() << "LocalCache: token write failed:" << m.lastError().text();
                if (inTransaction) db.rollback();
                return;
            }
        }
        m_syncToken = nextBatch;
        m_syncTokenAt = now;
    }

    if (inTransaction && !db.commit()) {
        qWarning() << "LocalCache: commit failed:" << db.lastError().text();
        db.rollback();
    }
}

QStringList LocalCache::cachedRoomIds() const
{
    QStringList out;
    if (!m_open) return out;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    if (!q.exec(QStringLiteral("SELECT room_id FROM rooms ORDER BY room_id"))) return out;
    while (q.next()) out.append(q.value(0).toString());
    return out;
}

bool LocalCache::buildHydrationSync(bsfchat::SyncResponse& out) const
{
    if (!m_open) return false;

    auto db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT room_id, json FROM room_state"))) {
        qWarning() << "LocalCache: snapshot read failed:" << q.lastError().text();
        return false;
    }

    int recovered = 0;
    int skipped = 0;
    while (q.next()) {
        const QString roomId = q.value(0).toString();
        if (roomId.isEmpty()) continue;
        bsfchat::RoomEvent ev;
        try {
            auto j = nlohmann::json::parse(q.value(1).toString().toStdString());
            bsfchat::from_json(j, ev);
        } catch (const std::exception&) {
            // One bad row must not cost us the whole snapshot.
            ++skipped;
            continue;
        }
        out.rooms.join[roomId.toStdString()].state.events.push_back(std::move(ev));
        ++recovered;
    }
    if (skipped > 0)
        qWarning() << "LocalCache:" << skipped << "unreadable state rows skipped";

    // Rooms recorded with no cached state still belong in the sidebar.
    QSqlQuery r(db);
    if (r.exec(QStringLiteral("SELECT room_id, unread_count FROM rooms"))) {
        while (r.next()) {
            const QString roomId = r.value(0).toString();
            if (roomId.isEmpty()) continue;
            auto& joined = out.rooms.join[roomId.toStdString()];
            joined.unread_count = r.value(1).toInt();
        }
    }

    // An empty snapshot is indistinguishable from a first run. Say so, so
    // the caller doesn't "resume" into a blank sidebar.
    if (out.rooms.join.empty() || recovered == 0) {
        out = bsfchat::SyncResponse{};
        return false;
    }

    // Deliberately left blank: the caller must not treat a replay as having
    // advanced the sync position.
    out.next_batch.clear();
    return true;
}
