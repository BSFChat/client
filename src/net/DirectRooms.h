#pragma once

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

// Which rooms are DMs, with whom, and which DMs are being created right now.
//
// Starting a DM used to be "POST /createRoom, always". Whether one already
// existed was a question each QML entry point was supposed to ask first, and
// three of the five did not. The two that did were still racy: the only thing
// that ever added to the DM map was a create *succeeding*, so a second click
// during a slow /createRoom saw an empty list and created a twin. And a DM the
// OTHER person opened was never in the map at all — nothing read the server's
// m.direct — so it sat in the channel tree and every entry point would happily
// open a second room with the same peer.
//
// This holds the three answers in one place so ServerConnection can ask them
// for every caller: is there already a room (roomWith), is one on its way
// (beginCreate / endCreate), and what does the server say (merge).
//
// Header-only and free of ServerConnection so it can be tested without a
// network, an event loop or a QSettings file.
namespace bsfchat::net {

class DirectRooms {
public:
    const QMap<QString, QString>& peers() const { return m_peers; }
    bool contains(const QString& roomId) const { return m_peers.contains(roomId); }
    QString peerOf(const QString& roomId) const { return m_peers.value(roomId); }

    // Returns true if this changed anything, i.e. the caller has something to
    // persist and announce.
    bool record(const QString& roomId, const QString& peer)
    {
        if (roomId.isEmpty() || peer.isEmpty()) return false;
        auto it = m_peers.find(roomId);
        if (it != m_peers.end() && it.value() == peer) return false;
        m_peers[roomId] = peer;
        return true;
    }

    // Fold in an m.direct map (peer -> room ids). Returns the room ids that
    // were added or re-labelled.
    //
    // Additive on purpose. m.direct is a full replacement in the spec, but an
    // older server sends none at all and ours omits it when there is nothing
    // to say, so "absent from this map" is not evidence that a locally known
    // DM stopped being one.
    QStringList merge(const std::map<std::string, std::vector<std::string>>& direct,
                      const QString& selfId)
    {
        QStringList changed;
        for (const auto& [peerStr, rooms] : direct) {
            const QString peer = QString::fromStdString(peerStr);
            if (peer == selfId) continue;
            for (const auto& room : rooms) {
                const QString roomId = QString::fromStdString(room);
                if (record(roomId, peer)) changed.append(roomId);
            }
        }
        return changed;
    }

    // The existing DM with `peer`, or empty. `usable` filters out rooms the
    // caller cannot open any more (left, deleted, pruned) so a stale entry
    // falls through to a fresh create instead of jumping into nothing.
    // Lowest room id wins among several, so legacy duplicates resolve to the
    // same room on every call.
    template <typename Usable>
    QString roomWith(const QString& peer, Usable usable) const
    {
        for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
            if (it.value() == peer && usable(it.key())) return it.key();
        }
        return {};
    }

    // In-flight guard. beginCreate returns false when a create for `peer` is
    // already outstanding — the caller must not send another. Whoever got
    // `true` owes exactly one endCreate, on success AND on failure; a guard
    // that survives an error would lock that peer out for the session.
    bool beginCreate(const QString& peer)
    {
        if (m_creating.contains(peer)) return false;
        m_creating.insert(peer);
        return true;
    }
    void endCreate(const QString& peer) { m_creating.remove(peer); }
    bool isCreating(const QString& peer) const { return m_creating.contains(peer); }

private:
    QMap<QString, QString> m_peers;   // roomId -> peer user id
    QSet<QString> m_creating;         // peers with a /createRoom outstanding
};

} // namespace bsfchat::net
