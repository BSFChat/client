#pragma once

#include <QList>
#include <functional>

// Index/pointer bookkeeping for ServerManager's list of connections,
// lifted out of ServerManager so it can be tested without a Settings
// object, a ServerConnection, an event loop or a network stack.
//
// It exists because the ordering here is the whole bug (U-C1): removing
// the active server used to `deleteLater()` the connection first and
// *then* ask setActiveServer() to move off it — which early-returned
// because the index hadn't changed, leaving `activeServer` pointing at a
// deleted object and never emitting activeServerChanged. The rules the
// roster enforces, in this order:
//
//   1. The active pointer/index are cleared the moment the connection
//      leaves the list, so nothing can observe a dangling pointer.
//   2. activeServerChanged fires whenever the pointer OR the index
//      moved — the index shifts on its own when a server *before* the
//      active one is removed, and `activeServerIndex` is a Q_PROPERTY
//      notified by that same signal.
//   3. Retiring the removed connection (disconnect + deleteLater) is the
//      last thing that happens, always after activeServerChanged.
//
// Everything Qt-specific is a hook supplied by the owner, so the roster
// itself has no dependency beyond QList.
template <typename Conn>
class ServerRoster {
public:
    struct Hooks {
        // Drop the sidebar row and the persisted entry for `index`.
        std::function<void(int index)> rowRemoved;
        // Persist the new active index (only called when it moved).
        std::function<void(int index)> persistIndex;
        // Emit activeServerChanged (pointer and/or index moved).
        std::function<void()> activeChanged;
        // Emit serverRemoved(index).
        std::function<void(int index)> serverRemoved;
        // disconnectFromServer() + deleteLater(). Always last.
        std::function<void(Conn* conn)> retire;
    };

    Hooks hooks;

    const QList<Conn*>& connections() const { return m_connections; }
    int count() const { return m_connections.size(); }
    bool isEmpty() const { return m_connections.isEmpty(); }
    Conn* at(int index) const {
        return (index >= 0 && index < m_connections.size()) ? m_connections[index] : nullptr;
    }
    int indexOf(Conn* conn) const { return m_connections.indexOf(conn); }

    Conn* active() const { return m_active; }
    int activeIndex() const { return m_activeIndex; }

    void append(Conn* conn) { m_connections.append(conn); }

    // In-place swap used by rebuildConnection(): same slot, new object.
    // Reports the active change itself so callers can't forget to.
    void replace(int index, Conn* conn)
    {
        if (index < 0 || index >= m_connections.size()) return;
        m_connections[index] = conn;
        if (m_activeIndex == index) {
            m_active = conn;
            if (hooks.activeChanged) hooks.activeChanged();
        }
    }

    void setActive(int index)
    {
        if (index < -1 || index >= m_connections.size()) return;
        Conn* next = (index >= 0) ? m_connections[index] : nullptr;
        if (m_activeIndex == index && m_active == next) return;

        m_activeIndex = index;
        m_active = next;
        if (hooks.persistIndex) hooks.persistIndex(index);
        if (hooks.activeChanged) hooks.activeChanged();
    }

    void remove(int index)
    {
        if (index < 0 || index >= m_connections.size()) return;

        Conn* conn = m_connections.takeAt(index);

        Conn* const prevActive = m_active;
        const int prevIndex = m_activeIndex;
        const bool removedActive = (prevIndex == index);

        // Rule 1: never leave the dangling pointer reachable, not even
        // for the duration of the model/settings updates below (which
        // re-enter QML through rowsRemoved).
        if (removedActive) {
            m_active = nullptr;
            m_activeIndex = -1;
        } else if (m_activeIndex > index) {
            m_activeIndex--;
        }

        if (hooks.rowRemoved) hooks.rowRemoved(index);

        if (removedActive) {
            // The neighbour that slid into this slot, else the new last
            // row, else nothing left to show.
            const int newIndex = m_connections.isEmpty()
                ? -1 : qMin(index, m_connections.size() - 1);
            m_activeIndex = newIndex;
            m_active = (newIndex >= 0) ? m_connections[newIndex] : nullptr;
        }

        if (m_activeIndex != prevIndex && hooks.persistIndex)
            hooks.persistIndex(m_activeIndex);
        // Rule 2: the index alone moving is still a change QML must see.
        if ((m_active != prevActive || m_activeIndex != prevIndex) && hooks.activeChanged)
            hooks.activeChanged();

        if (hooks.serverRemoved) hooks.serverRemoved(index);
        // Rule 3: last, so no handler above can run against a connection
        // that has already been told to die.
        if (hooks.retire) hooks.retire(conn);
    }

private:
    QList<Conn*> m_connections;
    Conn* m_active = nullptr;
    int m_activeIndex = -1;
};
