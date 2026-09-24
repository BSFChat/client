#pragma once

#include <QObject>
#include <QList>
#include <QStringList>

#include "identity/IdTokenAccount.h"
#include "net/ServerDiscovery.h"
#include "net/ServerRoster.h"

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
    // Who the identity provider said we are, the moment it said so — read
    // out of the id_token, so it is known before any homeserver has
    // answered and even for an account that belongs to no server at all.
    //
    // The sign-in UI shows this and offers "not you?". Without it the
    // browser-session shortcut is silent: if the system browser already
    // holds a session, the whole OIDC round trip completes with nothing on
    // screen and the client signs in as whoever that session belongs to.
    // `identityAccountName` is the human name (may be empty),
    // `identityAccountId` the provider's `sub` (the one to trust).
    Q_PROPERTY(QString identityAccountName READ identityAccountName
               NOTIFY identityAccountChanged)
    Q_PROPERTY(QString identityAccountId READ identityAccountId
               NOTIFY identityAccountChanged)

public:
    explicit ServerManager(Settings* settings, QObject* parent = nullptr);
    ~ServerManager() override;

    ServerListModel* servers() const { return m_serverListModel; }
    ServerConnection* activeServer() const { return m_roster.active(); }
    int activeServerIndex() const { return m_roster.activeIndex(); }
    // Count / lookup helpers for the NotificationManager (which needs to
    // wire every existing + future connection for inbound-message signals).
    // connectionAt is also invokable from QML so the server rail's
    // context menu can show per-server connection status.
    int connectionCount() const { return m_roster.count(); }
    Q_INVOKABLE ServerConnection* connectionAt(int index) const { return m_roster.at(index); }
    int indexOfConnection(ServerConnection* conn) const { return m_roster.indexOf(conn); }
    // The connection currently in a voice channel, or nullptr. Distinct
    // from activeServer(): the user can be in voice on server A while
    // browsing server B, and the screen/camera controllers must keep
    // pushing frames to A's engine regardless of which server has
    // sidebar focus.
    ServerConnection* voiceServer() const;

    // All four take whatever the user typed. Each resolves it first (see
    // ServerDiscovery: normalisation + .well-known), so a server entry is
    // never saved under the marketing domain the user happened to know.
    Q_INVOKABLE void addServer(const QString& url, const QString& username, const QString& password);
    Q_INVOKABLE void addServerWithOidc(const QString& url);
    // Nudge every connection's long poll. Wired to the application coming
    // back to the foreground; see ServerConnection::resyncNow().
    void resyncAll();
    // Mark every connection's open room read, if its timeline was parked at
    // the end. Driven by the application leaving the foreground — see
    // ServerConnection::markActiveRoomRead for why a phone needs this and a
    // desktop never did.
    void markActiveRoomsRead();
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
    //
    // Now dispatches on the connection's own verdict rather than always
    // rebuilding. It used to copy the existing access token onto the
    // replacement object unconditionally, so on 2026-09-19 — when the
    // server's access_tokens table was purged — the only "try again" button
    // in the product redialled /sync with the same dead bearer token and
    // never once touched /login.
    Q_INVOKABLE void reconnectServer(int index);
    // Discard the connection's credential and run the server's current
    // login flow. The recovery path for a purged, expired or revoked
    // session; safe to call on a healthy connection too.
    Q_INVOKABLE void reauthenticateServer(int index);

    bool viewingDms() const { return m_viewingDms; }
    Q_INVOKABLE void setViewingDms(bool v);

    QString identityAccountName() const { return m_identityAccount.name; }
    QString identityAccountId() const { return m_identityAccount.subject; }
    // Drop this process's identity session: the access token, the API
    // client bound to it, and the account we were told about. The "not
    // you?" affordance behind the signed-in confirmation.
    //
    // What it does NOT do, and cannot from here, is end the session the
    // system browser holds with the provider — that is a cookie in another
    // application, and re-prompting for it means adding `prompt=login` to
    // the authorize request, which lives in the OIDC flow. Until that
    // lands the UI says so in words rather than pretending.
    Q_INVOKABLE void forgetIdentitySession();

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

    // Local files currently on the system clipboard, as file:// URLs, so
    // the composer can paste an image or a file into the channel (U-M14).
    //
    // Two sources, in order: URLs the source app put on the clipboard
    // (a Finder / Explorer copy), and — when the clipboard holds raw
    // image data with no URL, which is what a screenshot or a
    // copy-image-from-browser gives you — a PNG spilled to a temp file so
    // the existing sendMediaMessage path, which reads bytes off disk, can
    // take it unchanged. Empty when the clipboard holds nothing
    // pasteable (e.g. plain text), which is the composer's signal to let
    // the normal text paste happen.
    Q_INVOKABLE QStringList clipboardFileUrls() const;

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
    // Leave every voice channel this process is in AND wait, briefly, for
    // the server to answer. Called from aboutToQuit; see VoiceQuit.h.
    Q_INVOKABLE void leaveAllVoice();
    // How long the quit path waits for the leaves to be answered. Long
    // enough for a LAN round trip, short enough not to be felt.
    static constexpr int kQuitLeaveTimeoutMs = 500;

signals:
    void activeServerChanged();
    void serverAdded(int index);
    void serverRemoved(int index);
    void loginError(const QString& serverUrl, const QString& error);
    void loginSuccess(const QString& serverUrl);
    // Kept for the existing QML bindings, and now honest: `passwordAvailable`
    // is true only when the server actually advertised m.login.password. It
    // used to be forced true on any error, which is how a 404 from a web
    // server produced a registration form. `url` is the RESOLVED homeserver.
    void loginFlowsChecked(const QString& url, bool oidcAvailable, const QString& providerUrl, bool passwordAvailable);
    // The full story, for the Add-server dialog. `outcome` is one of
    // "oidc" / "password" / "both" / "no_supported_flow" / "unreachable" /
    // "not_a_server" (bsfchat::loginFlowKindName). `redirected` means
    // .well-known sent us somewhere other than what was typed, and `note`
    // is a non-empty explanation only when discovery had to fall back from
    // a well-known file that named an unreachable homeserver.
    void serverProbed(const QString& requestedUrl, const QString& resolvedUrl,
                      const QString& outcome, const QString& providerUrl,
                      bool redirected, const QString& note);
    // Relayed from the connection at `index`, which has no way to tell the
    // UI which sidebar row it is. Manager-level so the re-auth dialog can
    // subscribe once, at startup, instead of being pre-bound to a
    // particular connection before the flow that needs it has started.
    void reauthPasswordRequired(int index, const QString& serverUrl,
                                const QString& userId);
    void reauthFailed(int index, const QString& serverUrl, const QString& error);
    void identityLoginComplete(const QStringList& serverUrls);
    void identityLoginFailed(const QString& error);
    // Sign-in worked; the account is simply not a member of anything yet.
    // Its own signal rather than a flavour of identityLoginFailed because
    // it is not a failure and must not be toasted as one: the account name
    // is known, nothing is wrong, and the only useful next step is to join
    // a server by address. Reported as an error it produced the D-M8
    // dead-end wearing different words.
    void identityHasNoServers();
    void identityAccountChanged();
    void viewingDmsChanged();

private:
    // The bodies of addServer / registerServer / addServerWithOidc, once a
    // homeserver URL has been settled on. Separated so the identity-sync
    // path — which already holds canonical URLs from /api/servers — can add
    // a dozen servers without a .well-known round trip each. They still
    // normalise, which costs nothing and is pure.
    void addServerResolved(const QString& url, const QString& username, const QString& password);
    void registerServerResolved(const QString& url, const QString& username, const QString& password);
    void addServerWithOidcResolved(const QString& url);

    void onLoginSuccess(ServerConnection* conn);
    // Write this connection's live credentials into its settings row,
    // updating in place when the row exists. onLoginSuccess used to APPEND
    // unconditionally, which is fine exactly once — a second login on the
    // same connection (i.e. any re-authentication) left the new token in a
    // duplicate row while the restore path kept reading the dead one.
    void persistCredentials(ServerConnection* conn);
    void onLoginFailed(ServerConnection* conn, const QString& error);
    // Replace m_connections[index] with a fresh ServerConnection to
    // `url`, carrying over the old connection's credentials. Emits
    // serverRemoved+serverAdded for the index so NotificationManager
    // and the screen/camera controllers rewire their per-connection
    // signal subscriptions.
    void rebuildConnection(int index, const QString& url);
    // Hooks up a ServerConnection so per-server UI state (unread dot, etc.)
    // in the sidebar tracks the connection's state.
    // "Are we already on this server?" — see net/ServerDedup.h for why
    // every add path has to ask, and what went wrong when three of the four
    // did not.
    int existingServerIndex(const QString& url, const QString& userId) const;
    bool adoptExistingServer(int index);
    void wireConnection(ServerConnection* conn);

    Settings* m_settings;
    ServerListModel* m_serverListModel;
    // Connection list + active pointer/index. See ServerRoster.h for why
    // the bookkeeping lives there rather than inline.
    ServerRoster<ServerConnection> m_roster;
    // URL normalisation + .well-known discovery + the login-flow probe.
    // Constructed in the ctor body because its fetch function parents a
    // QNetworkAccessManager to `this`.
    bsfchat::ServerDiscovery m_discovery;
    bool m_viewingDms = false;

    // Identity-first login state. m_identityClient runs the OIDC browser
    // flow against the identity service itself; m_identityApi talks to its
    // /api/servers endpoints with the resulting access token.
    IdentityClient* m_identityClient = nullptr;
    IdentityApiClient* m_identityApi = nullptr;
    QString m_identityUrl;
    QString m_identityAccessToken;
    bsfchat::IdTokenAccount m_identityAccount;
};
