#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "net/MatrixClient.h"
#include "model/RoomListModel.h"
#include "model/ServerListModel.h"
#include "core/Settings.h"
#include "identity/IdentityClient.h"
#include "identity/IdentityApiClient.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

ServerManager::ServerManager(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_serverListModel(new ServerListModel(this))
{
    // Restore saved servers
    auto saved = m_settings->savedServers();
    for (const auto& entry : saved) {
        auto* conn = new ServerConnection(entry.url, this);
        conn->setCredentials(entry.userId, entry.accessToken, entry.deviceId, entry.displayName);
        m_connections.append(conn);
        QUrl url(entry.url);
        QString serverName = url.host().isEmpty() ? entry.displayName : url.host();
        m_serverListModel->addServer(serverName, entry.url);
        wireConnection(conn);
    }

    int savedIndex = m_settings->activeServerIndex();
    if (savedIndex >= 0 && savedIndex < m_connections.size()) {
        setActiveServer(savedIndex);
    }
}

ServerManager::~ServerManager() = default;

void ServerManager::addServer(const QString& url, const QString& username, const QString& password)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url); // Temporary name until login
    wireConnection(conn);

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::loginSucceeded, this, [this, conn, index]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::loginFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    conn->login(username, password);
    emit serverAdded(index);

    // Auto-select if first server
    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::registerServer(const QString& url, const QString& username, const QString& password)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url);
    wireConnection(conn);

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::registerSucceeded, this, [this, conn, index]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::registerFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    conn->registerUser(username, password);
    emit serverAdded(index);

    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::checkLoginFlows(const QString& url)
{
    auto* tempClient = new MatrixClient(this);
    tempClient->setHomeserver(url);
    tempClient->getLoginFlows();

    connect(tempClient, &MatrixClient::loginFlowsResult, this, [this, url, tempClient](const QJsonArray& flows) {
        bool oidcAvailable = false;
        bool passwordAvailable = false;
        QString providerUrl;

        for (const auto& flowVal : flows) {
            QJsonObject flow = flowVal.toObject();
            QString type = flow.value("type").toString();
            if (type == "m.login.token") {
                oidcAvailable = true;
                providerUrl = flow.value("identity_provider").toString();
            } else if (type == "m.login.password") {
                passwordAvailable = true;
            }
        }

        emit loginFlowsChecked(url, oidcAvailable, providerUrl, passwordAvailable);
        tempClient->deleteLater();
    });

    connect(tempClient, &MatrixClient::loginError, this, [this, url, tempClient](const QString& /*error*/) {
        // On error, assume password-only
        emit loginFlowsChecked(url, false, QString(), true);
        tempClient->deleteLater();
    });
}

void ServerManager::addServerWithOidc(const QString& url)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url);
    wireConnection(conn);

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::loginSucceeded, this, [this, conn]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::loginFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    // Query login flows to get the identity_provider URL, then start OIDC
    auto* tempClient = new MatrixClient(this);
    tempClient->setHomeserver(url);
    tempClient->getLoginFlows();

    connect(tempClient, &MatrixClient::loginFlowsResult, this, [this, conn, tempClient](const QJsonArray& flows) {
        QString providerUrl;
        for (const auto& flowVal : flows) {
            QJsonObject flow = flowVal.toObject();
            if (flow.value("type").toString() == "m.login.token") {
                providerUrl = flow.value("identity_provider").toString();
                break;
            }
        }
        tempClient->deleteLater();

        if (providerUrl.isEmpty()) {
            emit loginError(conn->serverUrl(), "Server does not provide an identity provider URL");
            return;
        }
        conn->loginWithOidc(providerUrl);
    });

    connect(tempClient, &MatrixClient::loginError, this, [this, conn, tempClient](const QString& error) {
        tempClient->deleteLater();
        onLoginFailed(conn, "Failed to query login flows: " + error);
    });

    emit serverAdded(index);

    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::removeServer(int index)
{
    if (index < 0 || index >= m_connections.size()) return;

    auto* conn = m_connections.takeAt(index);
    conn->disconnectFromServer();
    conn->deleteLater();

    m_serverListModel->removeServer(index);
    m_settings->removeServer(index);

    if (m_activeServerIndex == index) {
        int newIndex = m_connections.isEmpty() ? -1 : qMin(index, m_connections.size() - 1);
        setActiveServer(newIndex);
    } else if (m_activeServerIndex > index) {
        m_activeServerIndex--;
        m_settings->setActiveServerIndex(m_activeServerIndex);
    }

    emit serverRemoved(index);
}

void ServerManager::updateServerUrl(int index, const QString& newUrl)
{
    if (index < 0 || index >= m_connections.size()) return;

    QString url = newUrl.trimmed();
    if (url.isEmpty()) return;
    // Accept a bare host[:port] — default to https, matching the
    // add-server flow. Trailing slashes break path concatenation in
    // MatrixClient, so strip them.
    if (!url.contains(QStringLiteral("://")))
        url = QStringLiteral("https://") + url;
    while (url.endsWith(QLatin1Char('/'))) url.chop(1);

    if (url == m_connections[index]->serverUrl()) return;

    // Persist first — the saved-servers array is index-aligned with
    // m_connections, and rebuildConnection reads nothing from it, so
    // order doesn't matter beyond crash-safety.
    auto saved = m_settings->savedServers();
    if (index < saved.size()) {
        auto entry = saved[index];
        entry.url = url;
        m_settings->updateServer(index, entry);
    }

    // Sidebar tile: show the new host until the server-name state
    // event comes back over sync and pushName overwrites it.
    QUrl parsed(url);
    QString name = parsed.host().isEmpty() ? url : parsed.host();
    m_serverListModel->updateServer(index, name, url);

    rebuildConnection(index, url);
}

void ServerManager::reconnectServer(int index)
{
    if (index < 0 || index >= m_connections.size()) return;
    rebuildConnection(index, m_connections[index]->serverUrl());
}

void ServerManager::rebuildConnection(int index, const QString& url)
{
    auto* old = m_connections[index];

    // A voice session can't survive its connection — leave cleanly so
    // peers get hangups instead of waiting out the dead-peer grace.
    if (old->inVoiceChannel()) old->leaveVoiceChannel();

    // Credentials come off the live object rather than settings so a
    // rebuild works even for a connection whose login predates this
    // process (restored) or whose settings row is missing.
    auto* conn = new ServerConnection(url, this);
    conn->setCredentials(old->userId(), old->accessToken(),
                         old->deviceId(), old->displayName());
    m_connections[index] = conn;
    wireConnection(conn);

    if (m_activeServerIndex == index) {
        m_activeServer = conn;
        emit activeServerChanged();
    }

    old->disconnectFromServer();
    old->deleteLater();

    // NotificationManager and the screen/camera controllers subscribe
    // per-connection; removed+added for the same index makes them drop
    // the dead object and wire the replacement.
    emit serverRemoved(index);
    emit serverAdded(index);
}

void ServerManager::setActiveServer(int index)
{
    if (index < -1 || index >= m_connections.size()) return;
    if (m_activeServerIndex == index) return;

    m_activeServerIndex = index;
    m_activeServer = (index >= 0) ? m_connections[index] : nullptr;
    m_settings->setActiveServerIndex(index);
    emit activeServerChanged();

    // Open the channel the user last had open on the server they just switched
    // to. Without this the only thing that ever restored a channel was the
    // QML shells' one-shot hook on the roomListModel's rowsInserted, which
    // fires while a server's list is first populating and therefore never
    // again — so every server except the one that happened to be syncing at
    // launch landed on the "Pick a channel" empty state. No-op when that
    // server already has a channel open, and defers itself if its channel
    // list hasn't arrived yet.
    if (m_activeServer) m_activeServer->restoreLastTextRoom();
}

ServerConnection* ServerManager::voiceServer() const
{
    // Normally at most one connection is in voice; if the user manages
    // to be in voice on two servers, the first (oldest) wins — the
    // controllers can only broadcast into one mesh anyway.
    for (auto* conn : m_connections) {
        if (conn && conn->inVoiceChannel()) return conn;
    }
    return nullptr;
}

void ServerManager::setViewingDms(bool v)
{
    if (m_viewingDms == v) return;
    m_viewingDms = v;
    emit viewingDmsChanged();
}

// Called on every wired connection's activeRoomIdChanged — keeps
// `viewingDms` true whenever the user is reading a DM, so the
// sidebar's @ chip stays highlighted even if they got there via a
// message-link deep-link or last-channel-restore that never went
// through the DM list.
static void syncViewingDmsForConnection(ServerManager* mgr,
                                         ServerConnection* conn)
{
    if (!mgr || !conn) return;
    if (conn != mgr->activeServer()) return;  // only care about the
                                              // foreground server
    QString rid = conn->activeRoomId();
    if (rid.isEmpty()) return;
    if (conn->isDirectRoom(rid)) {
        mgr->setViewingDms(true);
    }
}

QVariantList ServerManager::allDirectRooms() const
{
    QVariantList out;
    for (int i = 0; i < m_connections.size(); ++i) {
        ServerConnection* conn = m_connections[i];
        if (!conn) continue;
        QVariantList rooms = conn->directRooms();
        for (const QVariant& v : rooms) {
            QVariantMap m = v.toMap();
            m[QStringLiteral("serverUrl")] = conn->serverUrl();
            m[QStringLiteral("serverName")] = conn->serverName();
            m[QStringLiteral("serverIndex")] = i;

            // Unread pull: we don't currently expose per-room
            // unread count as a simple scalar on ServerConnection,
            // but the channel-list model tracks it. Look it up via
            // RoomListModel for this room id.
            int unread = 0;
            if (auto* rlm = conn->roomListModel()) {
                QString roomId = m.value("roomId").toString();
                for (int r = 0; r < rlm->rowCount(); ++r) {
                    auto rid = rlm->data(rlm->index(r),
                        RoomListModel::RoomIdRole).toString();
                    if (rid == roomId) {
                        unread = rlm->data(rlm->index(r),
                            RoomListModel::UnreadCountRole).toInt();
                        break;
                    }
                }
            }
            m[QStringLiteral("unreadCount")] = unread;

            out.append(m);
        }
    }
    std::sort(out.begin(), out.end(),
        [](const QVariant& a, const QVariant& b) {
            return a.toMap().value("lastMessageTime").toLongLong()
                 > b.toMap().value("lastMessageTime").toLongLong();
        });
    return out;
}

QVariantList ServerManager::searchKnownUsers(const QString& query,
                                               int limit) const
{
    if (limit <= 0) limit = 12;
    QVariantList out;
    // Per-connection fetch (they're cheap — bounded by members seen
    // in recent /sync) then merge-and-trim. We don't try to de-dup
    // across servers: the same MXID on two different servers is two
    // distinct chat identities, and a user might want to DM either.
    for (int i = 0; i < m_connections.size(); ++i) {
        ServerConnection* conn = m_connections[i];
        if (!conn) continue;
        auto hits = conn->searchKnownUsers(query, limit);
        for (QVariant& v : hits) {
            QVariantMap m = v.toMap();
            m[QStringLiteral("serverUrl")] = conn->serverUrl();
            m[QStringLiteral("serverName")] = conn->serverName();
            m[QStringLiteral("serverIndex")] = i;
            out.append(m);
            if (int(out.size()) >= limit) return out;
        }
    }
    return out;
}

void ServerManager::wireConnection(ServerConnection* conn)
{
    // Keep the sidebar's per-server unread dot in sync with this
    // connection's hasUnread. The index can shift as servers are added or
    // removed, so resolve it at signal-emission time.
    auto pushUnread = [this, conn]() {
        int idx = m_connections.indexOf(conn);
        if (idx < 0) return;
        m_serverListModel->setUnreadCount(idx, conn->hasUnread() ? 1 : 0);
    };
    connect(conn, &ServerConnection::hasUnreadChanged, this, pushUnread);
    pushUnread();

    // whoami reconciliation found a stale/corrupt persisted user id and
    // fixed it in memory — rewrite the stored entry so the correction
    // sticks across restarts. Only the identity fields change; tokens
    // stay as persisted.
    connect(conn, &ServerConnection::identityCorrected, this, [this, conn]() {
        int idx = m_connections.indexOf(conn);
        if (idx < 0) return;
        auto servers = m_settings->savedServers();
        if (idx >= servers.size()) return;
        auto entry = servers[idx];
        entry.userId = conn->userId();
        entry.displayName = conn->displayName();
        m_settings->updateServer(idx, entry);
    });

    // Keep the sidebar's label and tooltip in sync with the server-wide
    // name. serverName() falls back to hostname when no name is set.
    auto pushName = [this, conn]() {
        int idx = m_connections.indexOf(conn);
        if (idx < 0) return;
        m_serverListModel->updateServer(idx, conn->serverName(), conn->serverUrl());
    };
    connect(conn, &ServerConnection::serverNameChanged, this, pushName);

    // Same pattern for the server icon — push the resolved HTTP URL
    // into the list model's IconUrlRole whenever it changes.
    auto pushIcon = [this, conn]() {
        int idx = m_connections.indexOf(conn);
        if (idx < 0) return;
        m_serverListModel->setIconUrl(idx, conn->serverAvatarUrl());
    };
    connect(conn, &ServerConnection::serverAvatarUrlChanged, this, pushIcon);
    pushIcon();

    // When the active room becomes a DM (from any code path — user
    // click, notification deep-link, last-room-restore), flip into
    // DM view so the sidebar highlighting reflects reality.
    connect(conn, &ServerConnection::activeRoomIdChanged, this,
        [this, conn]() { syncViewingDmsForConnection(this, conn); });

    // Settings plumbed in so the mic gate can read voiceMode / PTT.
    conn->setSettings(m_settings);
}

void ServerManager::loginWithIdentity(const QString& identityUrl) {
    // Open the identity portal in the browser — the user logs in there,
    // and can see their server list. Future: auto-fetch the list and
    // connect each server programmatically.
    QString url = identityUrl.isEmpty() ? QStringLiteral("https://id.bsfchat.com") : identityUrl;
    QDesktopServices::openUrl(QUrl(url + "/profile.html"));
}

void ServerManager::registerServerMembership(const QString& identityUrl,
                                              const QString& serverUrl,
                                              const QString& serverName) {
    // Fire-and-forget POST to register this server with the identity provider.
    // Uses the identity session cookie (HttpOnly, set during OIDC).
    // In practice, QNetworkAccessManager doesn't share cookies with the
    // browser session, so this POST won't have the session. For a proper
    // implementation, the client would need its own identity access token.
    // Deferred to a follow-up — for now the identity portal's UI is the
    // source of truth for server membership.
    Q_UNUSED(identityUrl);
    Q_UNUSED(serverUrl);
    Q_UNUSED(serverName);
}

void ServerManager::onLoginSuccess(ServerConnection* conn)
{
    int index = m_connections.indexOf(conn);
    if (index < 0) return;

    // Use the server hostname as display name for the sidebar icon
    QUrl url(conn->serverUrl());
    QString serverDisplayName = url.host();
    if (serverDisplayName.isEmpty()) serverDisplayName = conn->serverUrl();
    m_serverListModel->updateServer(index, serverDisplayName, conn->serverUrl());
    emit loginSuccess(conn->serverUrl());

    Settings::ServerEntry entry;
    entry.url = conn->serverUrl();
    entry.userId = conn->userId();
    entry.accessToken = conn->accessToken();
    entry.deviceId = conn->deviceId();
    entry.displayName = conn->displayName();
    entry.identityProviderUrl = m_identityUrl;
    m_settings->addServer(entry);

    // If we have a live identity session, push this server to the user's
    // membership list so next launch auto-restores it. Fire and forget — if
    // the token has expired or the identity service is unreachable, it's
    // not worth failing the whole login over.
    if (m_identityApi && !m_identityAccessToken.isEmpty()) {
        m_identityApi->registerServer(conn->serverUrl(), serverDisplayName);
    }
}

void ServerManager::onLoginFailed(ServerConnection* conn, const QString& error)
{
    int index = m_connections.indexOf(conn);
    emit loginError(conn->serverUrl(), error);

    // Remove the failed connection
    if (index >= 0) {
        removeServer(index);
    }
}

void ServerManager::loginWithIdentityAndSync(const QString& identityUrl)
{
    // Normalise the identity URL. Empty means "default to the hosted
    // BSFChat identity service".
    QString normalized = identityUrl.trimmed();
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("https://id.bsfchat.com");
    }
    while (normalized.endsWith('/'))
        normalized.chop(1);
    m_identityUrl = normalized;

    // Reuse the existing OIDC PKCE browser flow. The identity service's
    // /authorize, /token etc. are the same endpoints IdentityClient already
    // talks to, so we can point it at the identity URL itself — the access
    // token we get back is just a session ID in the identity's session
    // table, which its /api/servers handlers accept as Bearer auth.
    if (!m_identityClient) {
        m_identityClient = new IdentityClient(this);
    } else {
        // Disconnect any stale signal connections from a previous attempt.
        m_identityClient->disconnect(this);
    }

    connect(m_identityClient, &IdentityClient::loginCompleted, this,
        [this](const QString& /*idToken*/, const QString& accessToken,
               const QString& refreshToken) {
            m_identityAccessToken = accessToken;

            // (Re)build the API client bound to this fresh token.
            if (m_identityApi) {
                m_identityApi->deleteLater();
                m_identityApi = nullptr;
            }
            m_identityApi = new IdentityApiClient(m_identityUrl, accessToken, this);

            connect(m_identityApi, &IdentityApiClient::serversFetched, this,
                [this](const QJsonArray& servers) {
                    QStringList addedUrls;
                    for (const auto& v : servers) {
                        QJsonObject obj = v.toObject();
                        QString serverUrl = obj.value("server_url").toString();
                        if (serverUrl.isEmpty()) continue;

                        // Skip servers we're already connected to — the user
                        // might re-run the sync while connections exist.
                        bool already = false;
                        for (auto* existing : m_connections) {
                            if (existing->serverUrl() == serverUrl) {
                                already = true;
                                break;
                            }
                        }
                        if (already) continue;

                        addedUrls.append(serverUrl);
                        addServerWithOidc(serverUrl);
                    }
                    emit identityLoginComplete(addedUrls);
                });

            connect(m_identityApi, &IdentityApiClient::fetchFailed, this,
                [this](const QString& error) {
                    emit identityLoginFailed(error);
                });

            m_identityApi->fetchServers();

            // Persist the refresh token so a future launch can silently
            // renew the identity session if we add it to that flow. We
            // store it against a placeholder entry keyed by identityUrl —
            // since ServerEntry already has identityRefreshToken/
            // identityProviderUrl fields, but those are per-server. For
            // now we just keep it in memory; MVP behaviour per the spec.
            Q_UNUSED(refreshToken);
        });

    connect(m_identityClient, &IdentityClient::loginFailed, this,
        [this](const QString& error) {
            emit identityLoginFailed(error);
        });

    m_identityClient->startLogin(m_identityUrl);
}

void ServerManager::leaveAllVoice()
{
    for (auto* conn : m_connections) {
        if (!conn) continue;
        // leaveVoiceChannel() no-ops when m_activeVoiceRoomId is
        // empty, so this is safe to fire on every connection even
        // if only one is actually in voice.
        conn->leaveVoiceChannel();
    }
}

void ServerManager::copyToClipboard(const QString& text)
{
    if (auto* cb = QGuiApplication::clipboard())
        cb->setText(text);
}

bool ServerManager::openMessageLink(const QString& link)
{
    // Expected: bsfchat://message/<percent-encoded server URL>/<roomId>/<eventId>
    const QString prefix = QStringLiteral("bsfchat://message/");
    if (!link.startsWith(prefix)) return false;
    const QString rest = link.mid(prefix.size());
    // Split on '/' from the right so the server URL (which itself may
    // contain encoded slashes) stays intact as the first segment.
    const int lastSlash = rest.lastIndexOf('/');
    if (lastSlash <= 0) return false;
    const int midSlash = rest.lastIndexOf('/', lastSlash - 1);
    if (midSlash <= 0) return false;
    const QString serverUrlEnc = rest.left(midSlash);
    const QString roomId = QUrl::fromPercentEncoding(
        rest.mid(midSlash + 1, lastSlash - midSlash - 1).toUtf8());
    const QString eventId = QUrl::fromPercentEncoding(
        rest.mid(lastSlash + 1).toUtf8());
    const QString serverUrl = QUrl::fromPercentEncoding(serverUrlEnc.toUtf8());

    // Find the matching connection by URL.
    int foundIdx = -1;
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i]->serverUrl() == serverUrl) {
            foundIdx = i;
            break;
        }
    }
    if (foundIdx < 0) return false;

    if (foundIdx != m_activeServerIndex)
        setActiveServer(foundIdx);
    // Delegate navigation to the connection (which owns the MessageModel).
    auto* conn = m_connections[foundIdx];
    if (conn) {
        // Deferred so MessageView can receive the scroll signal after any
        // room-switch signals have propagated.
        QMetaObject::invokeMethod(conn, [conn, roomId, eventId]() {
            if (conn->activeRoomId() != roomId)
                conn->setActiveRoom(roomId);
            QMetaObject::invokeMethod(conn, [conn, eventId]() {
                emit conn->scrollToEventRequested(eventId);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
    return true;
}
