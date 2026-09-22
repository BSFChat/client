#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <algorithm>

#include <bsfchat/Identifiers.h>

// The block list: `m.ignored_user_list`, the Matrix account-data document.
//
// Blocking on this server is the standard Matrix ignore list rather than a
// bsfchat.* invention — the server stores it at
// GET/PUT /_matrix/client/v3/user/{userId}/account_data/m.ignored_user_list
// and does all the enforcing itself (it filters ignored senders out of /sync,
// /messages, unread and mention counts, and push). The client needs the
// document for ONE thing: knowing who is on it, so it can draw "Blocked", offer
// "Unblock", and build the next version of the document.
//
// ── The PUT is a FULL REPLACEMENT ────────────────────────────────────────
//
// There is no "remove one entry" verb. Unblocking is writing the whole document
// again with that key left out, which means a client that builds the new
// document from a stale copy silently un-blocks everyone it has not heard about.
// That is the entire reason this class exists rather than a QSet<QString> on
// ServerConnection: the document we send is always derived from the document the
// server last gave us, unknown keys and all (documentWith / documentWithout),
// and adopting a server response is the only way the contents change.
//
// ── There is no live propagation, and this class does not pretend otherwise ──
//
// /sync carries no account_data section on this server, so a block made on
// another device does NOT arrive here. `loaded()` is "we have asked at least
// once", not "this is current"; the owner (ServerConnection) re-asks at the
// moments where being wrong would be visible — after login, after each of our
// own writes, and whenever the managed list is opened — and the UI says when it
// last looked. Nothing here schedules, polls or guesses.
//
// Header-only and free of ServerConnection, QObject and the network, so
// tests/test_ignored_users.cpp can pin the round trip — parse a document,
// add, remove, re-parse — without a socket or an event loop.
namespace bsfchat::net {

// The account-data type. Matrix's own, not a bsfchat.* name: a conventional
// client reading this account's block list looks here.
inline constexpr QLatin1StringView kIgnoredUserListType{"m.ignored_user_list"};
// The one key inside it. Its values are objects reserved by the spec for
// future use; this client preserves whatever is there rather than rewriting
// them to {} (see documentWith).
inline constexpr QLatin1StringView kIgnoredUsersKey{"ignored_users"};

// The server's ceiling, mirrored so the UI can refuse before the round trip
// rather than after it (server/src/api/InputLimits.h, kMaxIgnoredUsers).
inline constexpr int kMaxIgnoredUsers = 1000;

// GET/PUT /_matrix/client/v3/user/{userId}/account_data/{type}.
//
// Both segments are percent-encoded: a user id contains '@' and ':' and a type
// contains '.', and an unencoded ':' in a path segment is how "@a:b" became two
// segments and a 404 nobody could explain.
inline QString accountDataPath(const QString& userId, const QString& type)
{
    return QStringLiteral("/_matrix/client/v3/user/")
           + QString::fromUtf8(QUrl::toPercentEncoding(userId))
           + QStringLiteral("/account_data/")
           + QString::fromUtf8(QUrl::toPercentEncoding(type));
}

// Why a block was refused before it was sent. The server is still the
// authority — every one of these is also checked there and answered with a 400
// — but a user typing into a field deserves the answer without a round trip,
// and `AlreadyBlocked` in particular must not become a write that replaces the
// document with an identical one.
enum class BlockRefusal {
    None,
    NotAUserId,     // not @localpart:server
    Self,           // the server refuses a self-ignore, and so does this
    AlreadyBlocked, // nothing to write
    NotBlocked,     // unblocking somebody who is not on the list
    TooMany,        // at kMaxIgnoredUsers already
};

class IgnoredUsers {
public:
    // True once a document (or a 404 saying there is none) has come back.
    // NOT a claim that it is current — see the header comment.
    bool loaded() const { return m_loaded; }

    bool contains(const QString& userId) const
    {
        return m_ignored.contains(userId);
    }

    // Sorted, so the managed list does not reshuffle between refreshes
    // (QJsonObject iterates in its own order and the server may re-serialise).
    QStringList users() const
    {
        QStringList out = m_ignored.keys();
        std::sort(out.begin(), out.end());
        return out;
    }

    int count() const { return int(m_ignored.size()); }

    // A 200 from the GET, or the document we just successfully PUT read back.
    // Returns true when the membership changed, which is the caller's cue to
    // repaint; a re-fetch that agrees with what we had returns false.
    //
    // Everything outside `ignored_users` is KEPT, because the next PUT is built
    // from it. A future spec field, or a key another client wrote, must survive
    // a block made here rather than being quietly dropped.
    bool adopt(const QJsonObject& document)
    {
        const QJsonValue raw = document.value(QString(kIgnoredUsersKey));
        // A document with no `ignored_users`, or one whose value is not an
        // object, is an empty list — the same reading the server gives it.
        // Refusing to parse would leave the UI with no list at all over a
        // malformation nobody can fix from the client.
        const QJsonObject entries = raw.isObject() ? raw.toObject() : QJsonObject();

        const bool changed = (entries.keys() != m_ignored.keys());
        m_document = document;
        m_ignored = entries;
        m_loaded = true;
        return changed;
    }

    // The server answered 404: this account has never written the document.
    // Distinct from an empty one only in where it came from; both mean
    // "nobody is blocked" and both count as loaded.
    bool adoptAbsent()
    {
        const bool changed = !m_ignored.isEmpty();
        m_document = QJsonObject();
        m_ignored = QJsonObject();
        m_loaded = true;
        return changed;
    }

    // Connection dropped, or the account changed. A block list belongs to one
    // account on one server and must never be carried across either.
    void reset()
    {
        m_document = QJsonObject();
        m_ignored = QJsonObject();
        m_loaded = false;
    }

    // `selfId` may be empty (we have not learned our own id yet); the self
    // check is then skipped rather than guessed, and the server still catches it.
    BlockRefusal canBlock(const QString& userId, const QString& selfId) const
    {
        if (!bsfchat::UserId::is_valid(userId.toStdString())) return BlockRefusal::NotAUserId;
        if (!selfId.isEmpty() && userId == selfId) return BlockRefusal::Self;
        if (m_ignored.contains(userId)) return BlockRefusal::AlreadyBlocked;
        if (count() >= kMaxIgnoredUsers) return BlockRefusal::TooMany;
        return BlockRefusal::None;
    }

    BlockRefusal canUnblock(const QString& userId) const
    {
        if (!m_ignored.contains(userId)) return BlockRefusal::NotBlocked;
        return BlockRefusal::None;
    }

    // The document to PUT to add `userId`. The caller does NOT apply it
    // locally — the list changes when the server's answer is adopted, so a
    // write the server refuses cannot leave the UI claiming a block that is
    // not in force.
    //
    // A user already on the list keeps their existing value object rather than
    // being reset to {}: the spec reserves that object, and clobbering it would
    // make this client destroy a field it simply does not understand yet.
    QJsonObject documentWith(const QString& userId) const
    {
        return documentWithChanges({{userId, true}});
    }

    QJsonObject documentWithout(const QString& userId) const
    {
        return documentWithChanges({{userId, false}});
    }

    // Several changes at once, as ONE document.
    //
    // This is the shape the writer actually uses, and the two above are
    // conveniences over it. It matters because the PUT is a full replacement
    // and there can only usefully be one in flight: two overlapping writes,
    // each built from the same pre-write list, land in an order nobody
    // controls and the loser's change simply disappears. So a caller that has
    // three blocks to make sends one document with three entries, not three
    // documents with one each. BlockedUsersModel::pump is that caller.
    QJsonObject documentWithChanges(const QHash<QString, bool>& changes) const
    {
        QJsonObject entries = m_ignored;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            if (it.value()) {
                if (!entries.contains(it.key())) entries.insert(it.key(), QJsonObject());
            } else {
                entries.remove(it.key());
            }
        }
        return withEntries(entries);
    }

    // How many entries `changes` would leave, without building the document.
    // The ceiling is checked against this rather than against count(), because
    // a batch can cross it when none of its members would on their own.
    int countWithChanges(const QHash<QString, bool>& changes) const
    {
        int n = int(m_ignored.size());
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            const bool present = m_ignored.contains(it.key());
            if (it.value() && !present) ++n;
            else if (!it.value() && present) --n;
        }
        return n;
    }

private:
    QJsonObject withEntries(const QJsonObject& entries) const
    {
        QJsonObject doc = m_document;
        doc.insert(QString(kIgnoredUsersKey), entries);
        return doc;
    }

    // The last document the server gave us, whole. The source for the next PUT.
    QJsonObject m_document;
    // Its `ignored_users` map, lifted out for lookup.
    QJsonObject m_ignored;
    bool m_loaded = false;
};

} // namespace bsfchat::net
