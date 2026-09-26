#include "model/MemberListModel.h"

#include <QDateTime>
#include <QStringList>
#include <QTimer>

#include <bsfchat/Constants.h>

namespace {
// How often the release edge is checked. A quarter of kSpeakingHoldMs, so a
// member stops being "speaking" within 100 ms of when they are due to — under
// the ~200 ms a listener notices — without waking the event loop at the
// 50 Hz the levels themselves arrive at.
constexpr int kSpeakingExpiryTickMs = 100;
}

MemberListModel::MemberListModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_speakingExpiry(new QTimer(this))
{
    m_speakingExpiry->setInterval(kSpeakingExpiryTickMs);
    connect(m_speakingExpiry, &QTimer::timeout, this, [this] {
        expireSpeakingAt(QDateTime::currentMSecsSinceEpoch());
        if (m_speakingUntil.isEmpty()) m_speakingExpiry->stop();
    });
}

int MemberListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_members.size();
}

QVariant MemberListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_members.size())
        return {};

    const auto& member = m_members[index.row()];
    switch (role) {
    case UserIdRole: return member.userId;
    case DisplayNameRole: return resolveName(member.userId, member.displayName);
    case AvatarUrlRole: return member.avatarUrl;
    case MembershipRole: return member.membership;
    case NicknameRole: return member.nickname;
    case IsBotRole: return member.isBot;
    case IsSpeakingRole: return m_speakingUntil.contains(member.userId);
    default: return {};
    }
}

QString MemberListModel::resolveName(const QString& userId, const QString& localName) const
{
    if (!localName.isEmpty()) return localName;
    if (m_dnCache) {
        auto it = m_dnCache->find(userId);
        if (it != m_dnCache->end() && !it->isEmpty()) return *it;
    }
    // Strip @localpart:host → localpart as final fallback.
    if (userId.startsWith('@')) {
        int colon = userId.indexOf(':');
        if (colon > 1) return userId.mid(1, colon - 1);
    }
    return userId;
}

void MemberListModel::refreshDisplayNames()
{
    if (m_members.isEmpty()) return;
    // Tell views to re-read DisplayNameRole for every row.
    emit dataChanged(index(0), index(m_members.size() - 1), {DisplayNameRole});
}

bool MemberListModel::isBot(const QString& userId) const
{
    const int idx = findMember(userId);
    return idx >= 0 && m_members[idx].isBot;
}

QHash<int, QByteArray> MemberListModel::roleNames() const
{
    return {
        {UserIdRole, "userId"},
        {DisplayNameRole, "displayName"},
        {AvatarUrlRole, "avatarUrl"},
        {MembershipRole, "membership"},
        {NicknameRole, "nickname"},
        {IsBotRole, "isBot"},
        {IsSpeakingRole, "isSpeaking"}
    };
}

// ── Voice speaking state ────────────────────────────────────────────────
//
// See the block in the header for where the levels come from and why this
// collapses them to a bool before the view ever sees them.

void MemberListModel::setVoiceLevel(const QString& userId, float level)
{
    setVoiceLevelAt(userId, level, QDateTime::currentMSecsSinceEpoch());
}

void MemberListModel::setVoiceLevelAt(const QString& userId, float level,
                                      qint64 nowMs)
{
    if (userId.isEmpty()) return;
    // Below the floor is not an instruction to stop. The hold does that, so
    // that the silence between two words does not read as the end of a turn.
    if (level <= kSpeakingLevelFloor) return;

    const bool wasSpeaking = m_speakingUntil.contains(userId);
    m_speakingUntil[userId] = nowMs + kSpeakingHoldMs;
    // Only the RISING edge is a change worth telling the view about. Every
    // re-arm after that moves a deadline nobody can observe.
    if (!wasSpeaking) emitSpeakingChanged(userId);
    if (!m_speakingExpiry->isActive()) m_speakingExpiry->start();
}

int MemberListModel::expireSpeakingAt(qint64 nowMs)
{
    if (m_speakingUntil.isEmpty()) return 0;

    // Collected first, removed second: erasing from the hash while iterating
    // it is undefined, and emitSpeakingChanged() reads the hash back through
    // data() on any connected view.
    QStringList expired;
    for (auto it = m_speakingUntil.cbegin(); it != m_speakingUntil.cend(); ++it) {
        if (it.value() <= nowMs) expired.append(it.key());
    }
    for (const QString& userId : expired) {
        m_speakingUntil.remove(userId);
        emitSpeakingChanged(userId);
    }
    return int(expired.size());
}

void MemberListModel::clearVoiceLevel(const QString& userId)
{
    if (m_speakingUntil.remove(userId) > 0) emitSpeakingChanged(userId);
    if (m_speakingUntil.isEmpty()) m_speakingExpiry->stop();
}

void MemberListModel::clearVoiceLevels()
{
    if (m_speakingUntil.isEmpty()) return;
    const QStringList wereSpeaking = m_speakingUntil.keys();
    m_speakingUntil.clear();
    m_speakingExpiry->stop();
    for (const QString& userId : wereSpeaking) emitSpeakingChanged(userId);
}

bool MemberListModel::isSpeaking(const QString& userId) const
{
    return m_speakingUntil.contains(userId);
}

void MemberListModel::emitSpeakingChanged(const QString& userId)
{
    const int idx = findMember(userId);
    // A level can arrive for somebody who is in the call but not in the
    // roster of the room currently on screen. The state is kept either way —
    // it is keyed by user id, not by row — and there is simply no row to
    // repaint. Their row gets the right value from data() if they appear.
    if (idx < 0) return;
    emit dataChanged(index(idx), index(idx), {IsSpeakingRole});
}

int MemberListModel::findMember(const QString& userId) const
{
    for (int i = 0; i < m_members.size(); ++i) {
        if (m_members[i].userId == userId) return i;
    }
    return -1;
}

void MemberListModel::processEvent(const bsfchat::RoomEvent& event)
{
    if (event.type != std::string(bsfchat::event_type::kRoomMember))
        return;

    QString userId = event.state_key.has_value()
        ? QString::fromStdString(*event.state_key)
        : QString::fromStdString(event.sender);

    QString membership = QString::fromStdString(event.content.data.value("membership", ""));
    QString displayName = QString::fromStdString(event.content.data.value("displayname", ""));
    QString avatarUrl = QString::fromStdString(event.content.data.value("avatar_url", ""));
    // The server writes the EFFECTIVE name into `displayname` and the nickname
    // separately, so `displayName` above is already the string to render and this
    // is only for telling the two apart in admin UI.
    QString nickname = QString::fromStdString(
        event.content.data.value(std::string("bsfchat.nickname"), std::string()));
    // Absent is the server's encoding for "human" — it never writes `false` —
    // so a default of false is the whole of the rule, and there is no third
    // "not known yet" state to represent.
    bool isBotAccount = event.content.data.value(std::string("bsfchat.bot"), false);

    int idx = findMember(userId);

    if (membership == "join") {
        if (idx >= 0) {
            // Update existing member
            m_members[idx].displayName = displayName;
            m_members[idx].avatarUrl = avatarUrl;
            m_members[idx].membership = membership;
            m_members[idx].nickname = nickname;
            m_members[idx].isBot = isBotAccount;
            // U-M15: an empty role vector means "every role changed", which
            // makes every delegate binding on this row re-evaluate. Name the
            // five fields this branch can actually move.
            emit dataChanged(index(idx), index(idx),
                             {DisplayNameRole, AvatarUrlRole,
                              MembershipRole, NicknameRole, IsBotRole});
        } else {
            // Add new member
            beginInsertRows(QModelIndex(), m_members.size(), m_members.size());
            m_members.append({userId, displayName, avatarUrl, membership,
                              nickname, isBotAccount});
            endInsertRows();
        }
    } else if (membership == "leave" || membership == "ban") {
        if (idx >= 0) {
            beginRemoveRows(QModelIndex(), idx, idx);
            m_members.removeAt(idx);
            endRemoveRows();
        }
    }
}

QString MemberListModel::displayNameForUser(const QString& userId) const
{
    int idx = findMember(userId);
    if (idx >= 0) {
        return resolveName(userId, m_members[idx].displayName);
    }
    return {};
}

QString MemberListModel::nicknameForUser(const QString& userId) const
{
    int idx = findMember(userId);
    if (idx >= 0) return m_members[idx].nickname;
    return {};
}

void MemberListModel::clear()
{
    beginResetModel();
    m_members.clear();
    endResetModel();
}
