#pragma once

#include <QObject>
#include <QList>

class ServerConnection;
class ServerListModel;
class Settings;
class MatrixClient;
class IdentityClient;
class IdentityApiClient;

class ServerManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(ServerListModel* servers READ servers CONSTANT)
    Q_PROPERTY(ServerConnection* activeServer READ activeServer NOTIFY activeServerChanged)
    Q_PROPERTY(int activeServerIndex READ activeServerIndex WRITE setActiveServer NOTIFY activeServerChanged)
    // "Direct Messages" sits at the top of the server rail as a
    // server-shaped destination, even though DMs are hosted per-
    // server underneath. While viewingDms is true the channel panel
    // shows every DM across every connection; the active server +
    // active room are still set (so the composer knows where to
    // send), but the sidebar highlights the DM chip instead of the
    // hosting server. Cleared when the user picks a server icon.
    Q_PROPERTY(bool viewingDms READ viewingDms WRITE setViewingDms NOTIFY viewingDmsChanged)

public:
    explicit ServerManager(Settings* settings, QObject* parent = nullptr);
    ~ServerManager() override;

    ServerListModel* servers() const { return m_serverListModel; }
    ServerConnection* activeServer() const { return m_activeServer; }
    int activeServerIndex() const { return m_activeServerIndex; }
    // Count / lookup helpers for the NotificationManager (which needs to
    // wire every existing + future connection for inbound-message signals).
    // connectionAt is also invokable from QML so the server rail's
    // context menu can show per-server connection status.
    int connectionCount() const { return m_connections.size(); }
    Q_INVOKABLE ServerConnection* connectionAt(int index) const {
        return (index >= 0 && index < m_connections.size()) ? m_connections[index] : nullptr;
    }
    int indexOfConnection(ServerConnection* conn) const { return m_connections.indexOf(conn); }
    // The connection currently in a voice channel, or nullptr. Distinct
    // from activeServer(): the user can be in voice on server A while
    // browsing server B, and the screen/camera controllers must keep
    // pushing frames to A's engine regardless of which server has
    // sidebar focus.
    ServerConnection* voiceServer() const;

    Q_INVOKABLE void addServer(const QString& url, const QString& username, const QString& password);
    Q_INVOKABLE void addServerWithOidc(const QString& url);
    Q_INVOKABLE void checkLoginFlows(const QString& url);
    Q_INVOKABLE void registerServer(const QString& url, const QString& username, const QString& password);

    // Identity-first login: authenticate to the identity provider directly,
    // then fetch the user's server-membership list and auto-connect each.
    Q_INVOKABLE void loginWithIdentity(const QString& identityUrl);
    // After successfully connecting to a server via OIDC, register the
    // membership with the identity provider so future logins restore it.
    Q_INVOKABLE void registerServerMembership(const QString& identityUrl,
                                               const QString& serverUrl,
                                               const QString& serverName);
    Q_INVOKABLE void removeServer(int index);
    Q_INVOKABLE void setActiveServer(int index);
    // Repoint an already-added server at a new base URL (e.g. the
    // machine's IP changed, or http vs https was wrong). Persists the
    // new URL and rebuilds the connection with the saved credentials —
    // no re-login needed as long as the same server is behind the new
    // address.
    Q_INVOKABLE void updateServerUrl(int index, const QString& newUrl);
    // Tear down and rebuild the connection with its current URL +
    // credentials — the "it's stuck, kick it" affordance for a server
    // that won't reconnect on its own.
    Q_INVOKABLE void reconnectServer(int index);

    bool viewingDms() const { return m_viewingDms; }
    Q_INVOKABLE void setViewingDms(bool v);

    // Aggregate every connection's `directRooms()` into a single
    // flat list with the server's URL + human name stamped on each
    // row, newest-activity first. Used by the DM view in the
    // channel list so users see all their 1:1s at once regardless
    // of which server is technically hosting them.
    //
    // Each entry: {
    //   roomId, peerId, peerDisplayName, lastMessageTime,
    //   serverUrl, serverName, serverIndex, unreadCount
    // }
    Q_INVOKABLE QVariantList allDirectRooms() const;

    // Union of searchKnownUsers across every connection with the
    // hosting server's URL + index stamped on each row so the DM
    // composer can route the resulting createDirectMessage to the
    // correct connection. Same entry shape as ServerConnection's
    // plus `serverUrl`, `serverName`, `serverIndex`.
    Q_INVOKABLE QVariantList searchKnownUsers(const QString& query,
                                              int limit = 12) const;

    // Full identity-first flow: OIDC to the identity service itself, then
    // fetch the user's server list and auto-connect each one via OIDC.
    // identityUrl defaults to https://id.bsfchat.com when empty.
    Q_INVOKABLE void loginWithIdentityAndSync(const QString& identityUrl);

    // Copy arbitrary text to the system clipboard. Exposed on ServerManager
    // (rather than a separate helper) because QML already has it injected
    // and adding a second context property is noisier than adding a method.
    Q_INVOKABLE void copyToClipboard(const QString& text);

    // Parse a `bsfchat://message/<urlenc-server>/<roomId>/<eventId>` link and
    // navigate the UI to it: switch to the owning server, open the room, and
    // ask MessageView to scroll to the event. Returns false if no matching
    // server is connected.
    Q_INVOKABLE bool openMessageLink(const QString& link);

    // Best-effort voice disconnect for every connection that's currently
    // in a voice channel. Called on application shutdown / Android
    // onStop so the server doesn't keep stale presence for a client
    // whose process just died. The HTTP leave request is fire-and-
    // forget — if the OS kills us mid-flight, the server's ICE
    // timeout will eventually catch the corpse.
    Q_INVOKABLE void leaveAllVoice();

signals:
    void activeServerChanged();
    void serverAdded(int index);
    void serverRemoved(int index);
    void loginError(const QString& serverUrl, const QString& error);
    void loginSuccess(const QString& serverUrl);
    void loginFlowsChecked(const QString& url, bool oidcAvailable, const QString& providerUrl, bool passwordAvailable);
    void identityLoginComplete(const QStringList& serverUrls);
    void identityLoginFailed(const QString& error);
    void viewingDmsChanged();

private:
    void onLoginSuccess(ServerConnection* conn);
    void onLoginFailed(ServerConnection* conn, const QString& error);
    // Replace m_connections[index] with a fresh ServerConnection to
    // `url`, carrying over the old connection's credentials. Emits
    // serverRemoved+serverAdded for the index so NotificationManager
    // and the screen/camera controllers rewire their per-connection
    // signal subscriptions.
    void rebuildConnection(int index, const QString& url);
    // Hooks up a ServerConnection so per-server UI state (unread dot, etc.)
    // in the sidebar tracks the connection's state.
    void wireConnection(ServerConnection* conn);

    Settings* m_settings;
    ServerListModel* m_serverListModel;
    QList<ServerConnection*> m_connections;
    ServerConnection* m_activeServer = nullptr;
    int m_activeServerIndex = -1;
    bool m_viewingDms = false;

    // Identity-first login state. m_identityClient runs the OIDC browser
    // flow against the identity service itself; m_identityApi talks to its
    // /api/servers endpoints with the resulting access token.
    IdentityClient* m_identityClient = nullptr;
    IdentityApiClient* m_identityApi = nullptr;
    QString m_identityUrl;
    QString m_identityAccessToken;
};
