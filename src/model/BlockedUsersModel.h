#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>

#include "net/IgnoredUsers.h"
#include "net/MatrixFailure.h"

// View-model behind blocking: the menu entries that toggle a block, and the
// managed list in settings that shows who is on it.
//
// It lives apart from ServerConnection for the reason SelfRoleModel and
// BotAdminModel give — everything interesting here is a small state machine
// over replies that can arrive late or contradict what the UI already drew, and
// ServerConnection cannot be instantiated in a unit test. The network is
// reached through `Hooks` (std::function), so tests/test_ugc_safety.cpp
// substitutes a recording fake and delivers the replies by hand.
//
// ── One write at a time, because the PUT is a full replacement ───────────
//
// PUT /account_data/m.ignored_user_list replaces the WHOLE document. There is
// no add-one verb, which makes two overlapping writes actively destructive
// rather than merely wasteful: both are built from the same pre-write list,
// they land in an order nobody controls, and the loser's change is simply gone.
// Three quick clicks down a spam thread would block one person.
//
// So this model never has more than one write outstanding. Intended changes
// accumulate in `m_intent`, and `pump()` turns whatever is outstanding into ONE
// document when the line is clear. That makes three fast clicks one request
// carrying three entries, and it makes a click during a slow round trip wait
// for it rather than race it.
//
// The same rule covers the cold start, which is the dangerous end of it: until
// the list has been READ, there is nothing to build a full replacement from, so
// pump() fetches instead of writing and the intent waits. A client that skipped
// that step would send a document holding one id and erase every block the
// account already had — silently, and visibly only weeks later when the people
// they blocked started speaking again.
//
// ── Where the list comes from ────────────────────────────────────────────
//
// /sync carries an account_data section now (server schema v30,
// server/docs/read-state.md), so a block made on another device DOES arrive
// here, within a poll, through onDocumentFromSync. Nothing in this class
// polls: a poller would spend a request a minute on a document that changes
// twice a year, and the push has made it pointless as well as wasteful.
//
// The explicit re-asks below are kept, because the push is a delta and a
// client that was not running for it has nothing to replay from — and because
// they are what the pane's "Checked N ago" is honest about:
//
//   * after login / reconnect            (ServerConnection::refreshBlockedUsers)
//   * after every successful write of our own — the list is replaced with what
//     the SERVER now holds, not with what we asked for, so a write that raced
//     another device's write corrects itself instead of being papered over
//   * when the managed list is opened, and whenever the user presses Refresh
//
// `lastRefreshedMs` is what the settings pane renders ("Checked 4 minutes
// ago"), and a document pushed by /sync updates it like any other: the claim
// is "this is what the server last told us", and a push is the server telling
// us.
//
// ── Optimism, and its limit ──────────────────────────────────────────────
//
// A user with an outstanding intent reads back as the value we ASKED for, so a
// menu does not flicker between click and reply. That optimism covers what the
// UI draws and nothing else: the LIST only ever changes by adopting a server
// response, so a refusal drops the intent and the row returns to the last thing
// the server said. The server is the authority and onWriteFailed() honours it.
class BlockedUsersModel : public QObject {
    Q_OBJECT

    // One entry per blocked account, sorted by user id:
    //   { userId, displayName, pending }
    // Includes accounts with an outstanding intent, so a block made from a menu
    // appears here at once and a pending unblock stays visible until it lands
    // rather than vanishing and coming back on a refusal.
    Q_PROPERTY(QVariantList users READ users NOTIFY usersChanged)
    // True while any read or write is in flight, or anything is waiting to be
    // written.
    Q_PROPERTY(bool busy READ busy NOTIFY usersChanged)
    // True once the list has been fetched at least once. Distinguishes "nobody
    // is blocked" (empty state) from "we have not asked yet" (say nothing),
    // which look identical from an empty list.
    Q_PROPERTY(bool loaded READ loaded NOTIFY usersChanged)
    // When the list was last read from the server, ms since epoch; 0 when
    // never. The settings pane renders this verbatim — see the header for why
    // it is the only currency claim this client is entitled to make.
    Q_PROPERTY(qint64 lastRefreshedMs READ lastRefreshedMs NOTIFY usersChanged)
    Q_PROPERTY(int count READ count NOTIFY usersChanged)
    // Last refusal, empty when nothing has failed since it was dismissed.
    // Rendered inline in the pane rather than only as a toast, so the reason a
    // row sprang back sits where the row is.
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit BlockedUsersModel(QObject* parent = nullptr);

    // The network seam. ServerConnection installs the real calls; the test
    // records them. An unset hook makes the action a no-op rather than a crash
    // — which is what a pane opened against a dead connection should do.
    //
    // `store` receives the COMPLETE replacement document; see
    // net/IgnoredUsers.h for why there is no add/remove verb.
    struct Hooks {
        std::function<void()> fetch;
        std::function<void(const QString& requestId, const QJsonObject& document)> store;
    };
    Hooks hooks;

    // Injected so tests can pin "checked N ago" without sleeping.
    // Defaults to the wall clock.
    using Clock = std::function<qint64()>;
    void setClock(Clock clock) { m_clock = std::move(clock); }

    // Resolves a user id to the name to show in the managed list. Blocked
    // users are filtered out of sync, so for most of them the only thing this
    // client still knows is the id — the resolver returns an empty string then
    // and the pane shows the id, which is the honest answer rather than a
    // stale display name from before the block.
    void setDisplayNameResolver(std::function<QString(const QString&)> resolver)
    {
        m_resolveName = std::move(resolver);
    }

    // Our own user id, so a self-block is refused before it is sent. Empty
    // until whoami answers, in which case the check is skipped and the server
    // catches it.
    void setSelfUserId(const QString& userId) { m_selfId = userId; }

    QVariantList users() const { return m_view; }
    bool busy() const
    {
        return m_fetching || !m_inFlightId.isEmpty() || !m_intent.isEmpty();
    }
    bool loaded() const { return m_list.loaded(); }
    qint64 lastRefreshedMs() const { return m_lastRefreshedMs; }
    // Counting outstanding intents, so the settings row's badge agrees with
    // the list under it.
    int count() const { return m_list.countWithChanges(m_intent); }
    QString errorText() const { return m_errorText; }

    // ── questions (called from QML, on every paint of a menu) ─────────────

    // Whether `userId` is blocked, counting an outstanding intent as having
    // already landed. Never asks the network.
    Q_INVOKABLE bool isBlocked(const QString& userId) const;
    // Whether a change for `userId` has not been confirmed by the server yet,
    // so a row can say "Unblocking…" rather than pretending it is done.
    Q_INVOKABLE bool isPending(const QString& userId) const
    {
        return m_intent.contains(userId);
    }

    // ── actions (called from QML) ─────────────────────────────────────────

    // Re-read the list. Cheap and idempotent; a second call while one is in
    // flight is dropped.
    Q_INVOKABLE void refresh();
    // Ask for `userId` to be blocked / unblocked. Records the intent and lets
    // pump() decide when it goes out — which may be immediately, after the
    // list has been read, or after a write already in flight has landed. A
    // request for the state the list is already in records nothing.
    Q_INVOKABLE void block(const QString& userId);
    Q_INVOKABLE void unblock(const QString& userId);
    Q_INVOKABLE void dismissError();

    // ── replies ───────────────────────────────────────────────────────────

    // A 200 from the GET, or the document read back after a write.
    void onDocument(const QJsonObject& document);
    // The document as /sync delivered it — a write made on ANOTHER DEVICE,
    // arriving without anybody here having asked.
    //
    // Adopted only when this client has nothing outstanding. A document that
    // /sync built before our own PUT landed is older than what we asked for,
    // and adopting it would put a user we just unblocked back in the list
    // until the reply arrived — a visible flicker, and, worse, the base that
    // the NEXT full replacement would be derived from. Our own write's reply
    // adopts the server's answer anyway, and it is the newer one. Dropping the
    // pushed copy therefore costs nothing.
    //
    // Returns whether it was adopted, so a caller can log the difference.
    bool onDocumentFromSync(const QJsonObject& document);
    // A 404 from the GET: this account has never written a block list.
    void onDocumentAbsent();
    // The GET failed. The list keeps whatever it had — a failed refresh must
    // not empty a pane that was showing the truth a minute ago. Outstanding
    // intents survive when the list HAS been read before (they can still be
    // written from it) and are dropped when it has not, because there is
    // nothing safe to build a full replacement from.
    void onFetchFailed(const bsfchat::net::MatrixFailure& failure);
    // A write landed. `document` is what we sent, which the server has now
    // stored; adopting it is what makes the list and the next PUT agree.
    void onWriteStored(const QString& requestId, const QJsonObject& document);
    // A write was refused. Every intent it carried is dropped and the rows
    // return to the last known truth.
    void onWriteFailed(const QString& requestId,
                       const bsfchat::net::MatrixFailure& failure);

    // Connection dropped / server switched. A block list belongs to one
    // account on one server.
    void reset();

signals:
    void usersChanged();
    void errorTextChanged();
    // A change landed. `blocked` says which way it went, so the caller can
    // raise the right toast. Carried rather than left to the caller's memory
    // of what it asked for, because a reply can land after the menu closed —
    // and because one reply can carry several changes.
    void blockChanged(const QString& userId, bool blocked);

private:
    void rebuild();
    void setErrorText(const QString& text);
    // Send the outstanding intents as one document, if the line is clear and
    // there is a list to build from. The ONE place hooks.store is called.
    void pump();
    // Drop intents that the current list already agrees with — an idempotent
    // no-op must not become a write that replaces the document with an
    // identical copy.
    void pruneSatisfiedIntents();
    bool effectiveBlocked(const QString& userId) const;

    bsfchat::net::IgnoredUsers m_list;

    // userId -> the state we want it in, for everything the server has not
    // confirmed. Presence IS "pending", and it is also the optimistic value
    // isBlocked() reports.
    QHash<QString, bool> m_intent;
    // The write in flight: its token, and the intents it carries. Empty token
    // means the line is clear.
    QString m_inFlightId;
    QHash<QString, bool> m_inFlightIntent;

    bool m_fetching = false;
    qint64 m_lastRefreshedMs = 0;
    quint64 m_nextRequestId = 1;

    QString m_selfId;
    QVariantList m_view;
    QString m_errorText;

    Clock m_clock;
    std::function<QString(const QString&)> m_resolveName;
};
