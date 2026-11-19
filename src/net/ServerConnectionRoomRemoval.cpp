// ServerConnection: "this room is gone for me" (audit R-1).
//
// Kept out of ServerConnection.cpp on purpose. Everything here is one concern
// — a room leaving this account's world without this device doing it — and
// the decisions are pure functions in net/RoomRemoval.h with their own test.
//
// The three ways a room is learned to be gone:
//   * rooms.leave in an incremental /sync (servers that send it),
//   * GET /joined_rooms, once per session after the first live sync and again
//     whenever /messages answers M_FORBIDDEN / M_NOT_FOUND (every server),
//   * the first-sync prune of a genuine initial sync (processSyncResponse),
// plus this device's own leave and delete. All of them end in
// dropRoomsGoneForMe().

#include "net/ServerConnection.h"

#include "model/MemberListModel.h"
#include "model/MessageModel.h"
#include "model/RoomListModel.h"
#include "net/MatrixClient.h"
#include "net/RoomRemoval.h"
#include "net/TokenedReply.h"
#include "store/LocalCache.h"

#include <QDebug>

namespace {
// A /joined_rooms request that has not answered in this long is presumed lost
// (MatrixClient deliberately emits nothing on failure), so a new trigger may
// replace it. The replaced request's answer is then ignored by id.
constexpr qint64 kJoinedRoomsStaleMs = 60 * 1000;
} // namespace

void ServerConnection::onLiveSync(const bsfchat::SyncResponse& response)
{
    // Copied before processing: the stash lives exactly as long as the
    // emission that brought us here, and processSyncResponse is long.
    const QVector<bsfchat::client::SyncLeave> leaves = m_client->currentSyncLeaves();

    updatePendingInvites(response);

    processSyncResponse(response);

    // An accepted invite opens once the joined room is in the list.
    if (!m_openAfterJoin.isEmpty() && m_roomListModel->hasRoom(m_openAfterJoin)) {
        const QString roomId = m_openAfterJoin;
        m_openAfterJoin.clear();
        setActiveRoom(roomId);
    }

    // AFTER the whole batch, so a room left and re-joined within it is
    // judged on everything the batch says (roomsToDropForSyncLeaves).
    if (!leaves.isEmpty()) {
        const QStringList drop =
            bsfchat::client::roomsToDropForSyncLeaves(leaves, response, m_userId);
        if (!drop.isEmpty()) {
            for (const auto& leave : leaves) {
                if (drop.contains(leave.roomId)) {
                    qInfo().noquote() << "[rooms] gone via sync:" << leave.roomId
                                      << "reason=" << (leave.reason.isEmpty()
                                                           ? QStringLiteral("(none)")
                                                           : leave.reason);
                }
            }
            dropRoomsGoneForMe(drop, RoomGoneCause::Server);
        }
    }

    // Servers that predate rooms.leave say nothing at all, and a resumed
    // session never prunes (its first sync is a delta). One /joined_rooms per
    // session covers both: rooms deleted or revoked while we were away.
    if (!m_reconciledThisSession) {
        m_reconciledThisSession = true;
        requestJoinedRoomsReconcile();
    }
}

void ServerConnection::updatePendingInvites(const bsfchat::SyncResponse& response)
{
    // The section restates the complete pending set (protocol header), so
    // this is a rebuild. An invite that stops appearing was answered — here
    // or on another device.
    QVector<bsfchat::client::PendingInvite> now;
    QSet<QString> ids;
    for (const auto& [rid, room] : response.rooms.invite) {
        const QString roomId = QString::fromStdString(rid);
        now.append(bsfchat::client::pendingInviteFrom(roomId, room, m_userId));
        ids.insert(roomId);
    }
    // Answers this device sent and the server has now settled.
    for (auto it = m_inviteAnswersInFlight.begin(); it != m_inviteAnswersInFlight.end();) {
        if (!ids.contains(*it) && *it != m_openAfterJoin) it = m_inviteAnswersInFlight.erase(it);
        else ++it;
    }

    const QStringList fresh = bsfchat::client::newlyInvited(m_announcedInviteIds, now);
    const bool changed = ids != m_pendingInviteIds
        || now.size() != m_pendingInvites.size()
        || !std::equal(now.begin(), now.end(), m_pendingInvites.begin(),
                       [](const auto& a, const auto& b) {
                           return a.roomId == b.roomId && a.roomName == b.roomName
                               && a.inviterName == b.inviterName && a.isDirect == b.isDirect;
                       });
    m_pendingInviteIds = ids;
    m_pendingInvites = now;
    // Forget announcements for invites that went away, so a re-invite toasts.
    m_announcedInviteIds.intersect(ids);
    if (changed) emit pendingInvitesChanged();

    for (const auto& inv : now) {
        if (!fresh.contains(inv.roomId)) continue;
        m_announcedInviteIds.insert(inv.roomId);
        emit inviteReceived(inv.roomId, inv.roomName,
                            inv.inviterName.isEmpty() ? inv.inviterId : inv.inviterName,
                            inv.isDirect);
    }
}

void ServerConnection::clearActiveRoomView()
{
    if (m_activeRoomId.isEmpty()) return;
    m_activeRoomId.clear();
    m_activeRoomName.clear();
    m_activeRoomTopic.clear();
    m_messageModel->clear();
    m_memberListModel->clear();
    emit activeRoomIdChanged();
    emit activeRoomNameChanged();
    emit activeRoomTopicChanged();
}

void ServerConnection::dropRoomsGoneForMe(const QStringList& roomIds, RoomGoneCause cause)
{
    bool listChanged = false;
    bool activeGone = false;
    QString activeName;
    bool activeWasDirect = false;
    bool voiceGone = false;

    for (const QString& roomId : roomIds) {
        if (roomId.isEmpty()) continue;
        if (roomId == m_activeRoomId) {
            activeGone = true;
            activeName = m_roomListModel->roomDisplayName(roomId);
            activeWasDirect = m_roomListModel->isDirect(roomId);
        }
        if (!m_activeVoiceRoomId.isEmpty() && roomId == m_activeVoiceRoomId) voiceGone = true;

        // The timeline window goes BEFORE any switch away from the room:
        // setActiveRoom persists the window of the room it leaves, which
        // would write the gone room straight back to disk.
        m_timelines.forget(roomId);
        // And the disk copy — state snapshot, unread row, timeline — or the
        // next launch hydrates the room back into the sidebar, and a resumed
        // session never prunes it.
        if (m_cacheWired) m_cache->forgetRoom(roomId);

        m_roomTyping.remove(roomId);
        m_membersFetchedAt.remove(roomId);
        m_sentReadMarkerTs.remove(roomId);
        m_roomPinnedEvents.remove(roomId);
        m_roomMembers.remove(roomId);

        if (m_roomListModel->hasRoom(roomId)) {
            m_roomListModel->removeRoom(roomId);
            listChanged = true;
        }
    }

    if (voiceGone) leaveVoiceChannel();

    if (activeGone) {
        clearActiveRoomView();
        if (cause == RoomGoneCause::Server) {
            QString text;
            if (activeName.isEmpty())
                text = tr("This channel is no longer available");
            else if (activeWasDirect)
                text = tr("%1 is no longer available").arg(activeName);
            else
                text = tr("#%1 is no longer available").arg(activeName);
            emitFeedback(text, QStringLiteral("warn"));
        }
        // The app's own landing choice: the remembered channel if it still
        // exists (it usually IS the gone one), else the first text channel.
        // Before the first sync has answered this arms a pending restore
        // that processSyncResponse retries at the end of the pass.
        restoreLastTextRoom();
    }

    if (listChanged || activeGone) {
        rebuildCategorizedRooms();
        refreshDirectRooms();
        refreshMemberSnapshots();
        const bool hadUnread = m_hasUnread;
        m_hasUnread = m_roomListModel->totalUnreadCount() > 0;
        if (m_hasUnread != hadUnread) emit hasUnreadChanged();
    }
}

QStringList ServerConnection::sidebarRoomIds() const
{
    QStringList ids;
    const int rows = m_roomListModel->rowCount();
    ids.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        ids.append(m_roomListModel->data(m_roomListModel->index(i),
                                         RoomListModel::RoomIdRole).toString());
    }
    return ids;
}

void ServerConnection::requestJoinedRoomsReconcile()
{
    // No credential, no request: a restored-but-expired entry must keep every
    // byte off the wire (setCredentials' contract).
    if (m_accessToken.isEmpty() || m_userId.isEmpty()) return;

    if (!m_joinedRoomsRequestId.isEmpty()) {
        if (m_joinedRoomsAskedAt.isValid()
            && m_joinedRoomsAskedAt.elapsed() < kJoinedRoomsStaleMs) {
            // Something happened after the outstanding request went out —
            // its answer may predate it. Ask again once it lands.
            m_joinedRoomsAgain = true;
            return;
        }
        // Presumed lost; replaced below, and its answer will not match.
    }

    m_joinedRoomsAgain = false;
    m_joinedRoomsRequestId = bsfchat::net::newRequestToken();
    const QStringList ids = sidebarRoomIds();
    m_joinedRoomsAskedWith = QSet<QString>(ids.begin(), ids.end());
    m_joinedRoomsAskedAt.start();
    m_client->getJoinedRooms(m_joinedRoomsRequestId);
}

void ServerConnection::onJoinedRoomsResult(const QString& requestId,
                                           const QStringList& roomIds)
{
    // Not ours: an earlier session's (re-login, possibly as another
    // account), or a request already replaced as lost.
    if (requestId.isEmpty() || requestId != m_joinedRoomsRequestId) return;
    m_joinedRoomsRequestId.clear();
    m_joinedRoomsAskedAt.invalidate();

    QVector<bsfchat::client::SidebarRoom> sidebar;
    QSet<QString> protectedIds;
    const int rows = m_roomListModel->rowCount();
    sidebar.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        const QModelIndex idx = m_roomListModel->index(i);
        bsfchat::client::SidebarRoom room;
        room.roomId = m_roomListModel->data(idx, RoomListModel::RoomIdRole).toString();
        room.isCategory = m_roomListModel->data(idx, RoomListModel::RoomTypeRole).toString()
            == QLatin1String("category");
        room.parentId = m_roomListModel->data(idx, RoomListModel::ParentIdRole).toString();
        if (!m_joinedRoomsAskedWith.contains(room.roomId)) protectedIds.insert(room.roomId);
        sidebar.append(room);
    }
    m_joinedRoomsAskedWith.clear();

    const QSet<QString> joined(roomIds.begin(), roomIds.end());
    const QStringList drop = bsfchat::client::roomsToDropAfterReconcile(
        sidebar, joined, m_pendingInviteIds, protectedIds);
    if (!drop.isEmpty()) {
        qInfo().noquote() << "[rooms] gone via /joined_rooms:" << drop.join(QLatin1Char(' '));
        dropRoomsGoneForMe(drop, RoomGoneCause::Server);
    }

    if (m_joinedRoomsAgain) {
        m_joinedRoomsAgain = false;
        requestJoinedRoomsReconcile();
    }
}

void ServerConnection::resetRoomReconcile()
{
    m_joinedRoomsRequestId.clear();
    m_joinedRoomsAskedWith.clear();
    m_joinedRoomsAskedAt.invalidate();
    m_joinedRoomsAgain = false;
    m_reconciledThisSession = false;
    m_pendingInviteIds.clear();
    if (!m_pendingInvites.isEmpty()) {
        m_pendingInvites.clear();
        emit pendingInvitesChanged();
    }
    m_inviteAnswersInFlight.clear();
    m_announcedInviteIds.clear();
    m_openAfterJoin.clear();
}
