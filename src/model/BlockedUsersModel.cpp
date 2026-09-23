#include "model/BlockedUsersModel.h"

#include <QDateTime>
#include <QVariantMap>

#include <algorithm>

using bsfchat::net::BlockRefusal;
using bsfchat::net::MatrixFailure;

BlockedUsersModel::BlockedUsersModel(QObject* parent)
    : QObject(parent)
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    rebuild();
}

bool BlockedUsersModel::effectiveBlocked(const QString& userId) const
{
    // The outstanding intent wins. A menu row that flipped to "Unblock" on
    // click must not flip back for the duration of the round trip and then
    // forward again when it lands.
    auto it = m_intent.constFind(userId);
    if (it != m_intent.constEnd()) return it.value();
    return m_list.contains(userId);
}

bool BlockedUsersModel::isBlocked(const QString& userId) const
{
    return effectiveBlocked(userId);
}

void BlockedUsersModel::refresh()
{
    if (m_fetching) return;
    if (!hooks.fetch) return;
    m_fetching = true;
    emit usersChanged();
    hooks.fetch();
}

void BlockedUsersModel::block(const QString& userId)
{
    // The two checks that need no list. Done here rather than in pump()
    // because they are answers to what the user just did, and a message about
    // them belongs next to the click — pump() may not run until a round trip
    // later.
    if (!bsfchat::UserId::is_valid(userId.toStdString())) {
        setErrorText(tr("“%1” is not a user id.").arg(userId));
        return;
    }
    if (!m_selfId.isEmpty() && userId == m_selfId) {
        setErrorText(tr("You cannot block yourself."));
        return;
    }
    // Already in the state asked for. Not an error — the only way here is a
    // stale menu — and emphatically not a write: a full replacement with
    // identical contents is a round trip that can only lose.
    if (effectiveBlocked(userId)) return;

    m_intent.insert(userId, true);
    setErrorText(QString());
    rebuild();
    pump();
}

void BlockedUsersModel::unblock(const QString& userId)
{
    if (!effectiveBlocked(userId)) return;
    m_intent.insert(userId, false);
    setErrorText(QString());
    rebuild();
    pump();
}

void BlockedUsersModel::pruneSatisfiedIntents()
{
    for (auto it = m_intent.begin(); it != m_intent.end();) {
        if (m_list.contains(it.key()) == it.value()) it = m_intent.erase(it);
        else ++it;
    }
}

void BlockedUsersModel::pump()
{
    // One write at a time. See the header: two overlapping full replacements
    // do not merge, they overwrite, and the loser's change is gone.
    if (!m_inFlightId.isEmpty()) return;
    if (m_intent.isEmpty()) return;

    // Nothing to build a full replacement from yet. Read first; onDocument /
    // onDocumentAbsent calls back into here, and the intent waits meanwhile.
    if (!m_list.loaded()) {
        refresh();
        return;
    }

    pruneSatisfiedIntents();
    if (m_intent.isEmpty()) {
        rebuild();
        return;
    }

    // The ceiling is checked against what the BATCH would leave, not against
    // the current count: a batch can cross it when no member of it would on
    // its own. Refused whole, because there is no honest way to pick which of
    // the user's blocks to drop on their behalf.
    if (m_list.countWithChanges(m_intent) > bsfchat::net::kMaxIgnoredUsers) {
        setErrorText(tr("You have blocked the maximum of %1 accounts. "
                        "Unblock someone to make room.")
                         .arg(bsfchat::net::kMaxIgnoredUsers));
        m_intent.clear();
        rebuild();
        return;
    }

    if (!hooks.store) return;

    m_inFlightId = QString::number(m_nextRequestId++);
    m_inFlightIntent = m_intent;
    const QJsonObject document = m_list.documentWithChanges(m_intent);
    rebuild();
    hooks.store(m_inFlightId, document);
}

void BlockedUsersModel::dismissError()
{
    setErrorText(QString());
}

void BlockedUsersModel::onDocument(const QJsonObject& document)
{
    m_fetching = false;
    m_list.adopt(document);
    m_lastRefreshedMs = m_clock();
    rebuild();
    // Anything the user asked for while we were reading goes out now, against
    // the list that just landed.
    pump();
}

bool BlockedUsersModel::onDocumentFromSync(const QJsonObject& document)
{
    // Anything outstanding means the newer truth is already on its way here.
    // See the header: adopting an older document would resurrect a change the
    // user has already made and then build the next replacement from it.
    if (m_fetching || !m_inFlightId.isEmpty() || !m_intent.isEmpty()) return false;
    onDocument(document);
    return true;
}

void BlockedUsersModel::onDocumentAbsent()
{
    m_fetching = false;
    m_list.adoptAbsent();
    m_lastRefreshedMs = m_clock();
    rebuild();
    pump();
}

void BlockedUsersModel::onFetchFailed(const MatrixFailure& failure)
{
    m_fetching = false;

    // An intent that has never had a list to build from cannot be written: the
    // document it would produce is a full replacement derived from nothing.
    // Dropped, and said so — the click costs the user a second one, which is
    // very much better than the alternative. Once the list HAS been read,
    // intents survive a failed refresh, because they can still be written from
    // the copy we hold.
    const bool strandedIntents = !m_list.loaded() && !m_intent.isEmpty();
    if (strandedIntents) m_intent.clear();

    // The list itself is left alone on purpose. A refresh that failed says
    // nothing about who is blocked, and emptying the pane over a dropped
    // connection would read as "your blocks are gone".
    if (failure.isRateLimited() && failure.retrySeconds() > 0) {
        setErrorText(tr("Too many requests — try again in %n second(s).", nullptr,
                        failure.retrySeconds()));
    } else if (strandedIntents) {
        setErrorText(tr("Nobody was blocked: your blocked list could not be "
                        "read (%1). Try again.").arg(failure.message));
    } else {
        setErrorText(tr("Could not read your blocked list: %1").arg(failure.message));
    }
    rebuild();
}

void BlockedUsersModel::onWriteStored(const QString& requestId,
                                      const QJsonObject& document)
{
    // A reply to a write this model no longer owns — a reset between request
    // and response, or a duplicate. Nothing to adopt: the document it
    // describes may belong to another account entirely.
    if (requestId.isEmpty() || requestId != m_inFlightId) return;
    m_inFlightId.clear();

    // Adopt what was STORED rather than applying our own intents to the local
    // set. The two agree when nothing raced, and when something did, this is
    // the version the server will serve to every future reader.
    m_list.adopt(document);
    m_lastRefreshedMs = m_clock();

    const QHash<QString, bool> carried = m_inFlightIntent;
    m_inFlightIntent.clear();
    // Retire only the intents this write carried, and only where the user has
    // not since asked for the opposite — a click during the round trip is a
    // newer wish than the one that just landed and must survive to the next
    // pump().
    for (auto it = carried.cbegin(); it != carried.cend(); ++it) {
        auto cur = m_intent.constFind(it.key());
        if (cur != m_intent.constEnd() && cur.value() == it.value())
            m_intent.remove(it.key());
    }
    rebuild();

    // Announced per user, because one document can carry several changes and
    // each is a separate thing the person did.
    QStringList names = carried.keys();
    std::sort(names.begin(), names.end());
    for (const QString& userId : names)
        emit blockChanged(userId, carried.value(userId));

    // Whatever arrived while that was in flight.
    pump();
}

void BlockedUsersModel::onWriteFailed(const QString& requestId,
                                      const MatrixFailure& failure)
{
    if (requestId.isEmpty() || requestId != m_inFlightId) return;
    m_inFlightId.clear();

    const QHash<QString, bool> carried = m_inFlightIntent;
    m_inFlightIntent.clear();
    // Roll back exactly what this write carried, leaving anything the user
    // asked for since. Dropping the intent IS the rollback: the rows fall back
    // to m_list, which is still the last thing the server said.
    bool anyBlock = false;
    for (auto it = carried.cbegin(); it != carried.cend(); ++it) {
        auto cur = m_intent.constFind(it.key());
        if (cur != m_intent.constEnd() && cur.value() == it.value())
            m_intent.remove(it.key());
        if (it.value()) anyBlock = true;
    }

    if (failure.isRateLimited() && failure.retrySeconds() > 0) {
        setErrorText(tr("Too many requests — try again in %n second(s).", nullptr,
                        failure.retrySeconds()));
    } else if (anyBlock) {
        setErrorText(tr("Could not update your blocked list: %1").arg(failure.message));
    } else {
        setErrorText(tr("Could not unblock: %1").arg(failure.message));
    }
    rebuild();
    pump();
}

void BlockedUsersModel::reset()
{
    m_list.reset();
    m_intent.clear();
    m_inFlightId.clear();
    m_inFlightIntent.clear();
    m_fetching = false;
    m_lastRefreshedMs = 0;
    setErrorText(QString());
    rebuild();
}

void BlockedUsersModel::rebuild()
{
    // The union of what the server holds and what we have asked for, so a
    // block made from a menu appears at once and a pending unblock stays
    // visible — a row that vanished on click and came back on a refusal is a
    // list that jumps under the cursor.
    QStringList ids = m_list.users();
    for (auto it = m_intent.cbegin(); it != m_intent.cend(); ++it) {
        if (it.value() && !ids.contains(it.key())) ids.append(it.key());
    }
    std::sort(ids.begin(), ids.end());

    QVariantList view;
    view.reserve(ids.size());
    for (const QString& id : ids) {
        QVariantMap row;
        row[QStringLiteral("userId")] = id;
        row[QStringLiteral("displayName")] = m_resolveName ? m_resolveName(id) : QString();
        row[QStringLiteral("pending")] = m_intent.contains(id);
        view.append(row);
    }
    m_view = view;
    emit usersChanged();
}

void BlockedUsersModel::setErrorText(const QString& text)
{
    if (m_errorText == text) return;
    m_errorText = text;
    emit errorTextChanged();
}
