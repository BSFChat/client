#include "net/ServerConnection.h"
#include "net/MatrixClient.h"
#include "net/SyncLoop.h"
#include "model/RoomListModel.h"
#include "model/MessageModel.h"
#include "model/MemberListModel.h"
#include "identity/IdentityClient.h"
#ifdef BSFCHAT_VOICE_ENABLED
#include "voice/VoiceEngine.h"
#include "voice/NotificationSounds.h"
#endif

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>

#include <bsfchat/Constants.h>
#include <nlohmann/json.hpp>

ServerConnection::ServerConnection(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_client(new MatrixClient(this))
    , m_syncLoop(new SyncLoop(m_client, this))
#ifdef BSFCHAT_VOICE_ENABLED
    , m_sounds(new NotificationSounds(this))
#endif
    , m_roomListModel(new RoomListModel(this))
    , m_messageModel(new MessageModel(this))
    , m_memberListModel(new MemberListModel(this))
    , m_serverUrl(serverUrl)
{
    m_client->setHomeserver(serverUrl);
    m_messageModel->setHomeserver(serverUrl);

    // Connect sync signals
    connect(m_syncLoop, &SyncLoop::syncCompleted, this, &ServerConnection::processSyncResponse);

    // Track sync errors for UI
    connect(m_syncLoop, &SyncLoop::syncError, this, [this](const QString& error) {
        m_connectionStatus = 2; // reconnecting
        m_syncErrorMessage = error;
        emit connectionStatusChanged();
        emit syncErrorMessageChanged();
    });

    // Connect room members response
    connect(m_client, &MatrixClient::roomMembersResult, this, [this](const QString& roomId, const QJsonArray& members) {
        if (roomId != m_activeRoomId) return;
        m_memberListModel->clear();
        for (const auto& memberVal : members) {
            auto obj = memberVal.toObject();
            QString type = obj.value("type").toString();
            if (type != QString::fromUtf8(bsfchat::event_type::kRoomMember)) continue;
            QString membership = obj.value("content").toObject().value("membership").toString();
            if (membership != "join") continue;

            QString userId = obj.value("state_key").toString();
            QString displayName = obj.value("content").toObject().value("displayname").toString();

            // Build a RoomEvent to feed into processEvent
            bsfchat::RoomEvent ev;
            ev.type = std::string(bsfchat::event_type::kRoomMember);
            ev.sender = userId.toStdString();
            ev.state_key = userId.toStdString();
            ev.content.data = {{"membership", "join"}};
            if (!displayName.isEmpty()) {
                ev.content.data["displayname"] = displayName.toStdString();
            }
            m_memberListModel->processEvent(ev);
        }
    });

    // Connect message history results
    connect(m_client, &MatrixClient::messagesResult, this, [this](const bsfchat::MessagesResponse& resp) {
        auto events = resp.chunk;
        std::reverse(events.begin(), events.end());
        for (const auto& event : events) {
            m_messageModel->appendEvent(event, m_userId);
        }
    });

    // Typing timers
    // Debounce timer: prevents sending typing=true more than once per 4 seconds
    m_typingTimer = new QTimer(this);
    m_typingTimer->setSingleShot(true);
    m_typingTimer->setInterval(4000);

    // Stop timer: sends typing=false after 5 seconds of no typing activity
    m_typingStopTimer = new QTimer(this);
    m_typingStopTimer->setSingleShot(true);
    m_typingStopTimer->setInterval(5000);
    connect(m_typingStopTimer, &QTimer::timeout, this, [this]() {
        if (!m_activeRoomId.isEmpty() && !m_userId.isEmpty()) {
            m_client->setTyping(m_activeRoomId, m_userId, false);
        }
    });

    // Voice poll timer
    m_voicePollTimer = new QTimer(this);
    m_voicePollTimer->setInterval(5000);
    connect(m_voicePollTimer, &QTimer::timeout, this, [this]() {
        if (!m_activeVoiceRoomId.isEmpty()) {
            m_client->getVoiceMembers(m_activeVoiceRoomId);
        }
    });

    // Voice signal connections
    connect(m_client, &MatrixClient::voiceJoined, this, [this](const QString& roomId, const QJsonArray& members) {
        m_activeVoiceRoomId = roomId;
        m_voiceMembers = members;
        m_voiceMuted = false;
        m_voiceDeafened = false;
        m_voicePollTimer->start();
        emit activeVoiceRoomIdChanged();
        emit voiceMembersChanged();
        emit voiceMutedChanged();
        emit voiceDeafenedChanged();
#ifdef BSFCHAT_VOICE_ENABLED
        m_sounds->playJoin();
#endif
        // Fetch TURN config to start WebRTC
        m_client->getTurnConfig();
    });

    connect(m_client, &MatrixClient::turnConfigResult, this, [this](const QJsonObject& config) {
#ifdef BSFCHAT_VOICE_ENABLED
        if (!m_activeVoiceRoomId.isEmpty() && !m_voiceEngine) {
            m_voiceEngine = new VoiceEngine(m_client, this);
            m_voiceEngine->start(m_activeVoiceRoomId, m_voiceMembers, config);
        }
#endif
    });

    connect(m_client, &MatrixClient::voiceLeft, this, [this](const QString& /*roomId*/) {
#ifdef BSFCHAT_VOICE_ENABLED
        m_sounds->playLeave();
#endif
        m_activeVoiceRoomId.clear();
        m_voiceMembers = QJsonArray();
        m_voiceMuted = false;
        m_voiceDeafened = false;
        m_voicePollTimer->stop();
        emit activeVoiceRoomIdChanged();
        emit voiceMembersChanged();
        emit voiceMutedChanged();
        emit voiceDeafenedChanged();
    });

    connect(m_client, &MatrixClient::voiceMembersResult, this, [this](const QString& roomId, const QJsonArray& members) {
        if (roomId == m_activeVoiceRoomId) {
            m_voiceMembers = members;
            emit voiceMembersChanged();
        }
        // Update voice member count in room list
        m_roomListModel->updateVoiceMemberCount(roomId, members.size());
    });

    connect(m_client, &MatrixClient::voiceChannelCreated, this, [this](const QString& /*roomId*/) {
        // Room will appear via sync; nothing extra needed
    });

    // Profile signal connections
    connect(m_client, &MatrixClient::displayNameUpdated, this, [this]() {
        // Re-fetch our own profile to update local state
        m_client->getProfile(m_userId);
    });

    connect(m_client, &MatrixClient::avatarUrlUpdated, this, [this]() {
        m_client->getProfile(m_userId);
    });

    connect(m_client, &MatrixClient::profileResult, this, [this](const QString& userId, const QString& displayName, const QString& avatarUrl) {
        // If this is our own profile, update local state
        if (userId == m_userId) {
            if (!displayName.isEmpty() && displayName != m_displayName) {
                m_displayName = displayName;
                emit displayNameChanged();
            }
            if (avatarUrl != m_avatarUrl) {
                m_avatarUrl = avatarUrl;
                emit avatarUrlChanged();
            }
        }
        emit profileFetched(userId, displayName, avatarUrl);
    });

    // Handle leave room success
    connect(m_client, &MatrixClient::leaveRoomSuccess, this, [this](const QString& roomId) {
        if (roomId == m_activeRoomId) {
            m_activeRoomId.clear();
            m_activeRoomName.clear();
            m_activeRoomTopic.clear();
            m_messageModel->clear();
            m_memberListModel->clear();
            emit activeRoomIdChanged();
            emit activeRoomNameChanged();
            emit activeRoomTopicChanged();
        }
    });
}

ServerConnection::~ServerConnection()
{
    disconnectFromServer();
}

void ServerConnection::setCredentials(const QString& userId, const QString& accessToken,
                                       const QString& deviceId, const QString& displayName)
{
    m_userId = userId;
    m_accessToken = accessToken;
    m_deviceId = deviceId;
    m_displayName = displayName;
    m_client->setAccessToken(accessToken);
    m_connected = true;
    m_connectionStatus = 1;
    startSync();
}

void ServerConnection::login(const QString& username, const QString& password)
{
    QObject::connect(m_client, &MatrixClient::loginSuccess, this,
        [this](const bsfchat::LoginResponse& resp) {
            m_userId = QString::fromStdString(resp.user_id);
            m_accessToken = QString::fromStdString(resp.access_token);
            m_deviceId = QString::fromStdString(resp.device_id);
            m_displayName = m_userId;
            m_client->setAccessToken(m_accessToken);
            m_connected = true;
            m_connectionStatus = 1;
            emit userIdChanged();
            emit displayNameChanged();
            emit connectedChanged();
            emit connectionStatusChanged();
            emit loginSucceeded();
            startSync();
        }, Qt::SingleShotConnection);

    QObject::connect(m_client, &MatrixClient::loginError, this,
        [this](const QString& error) {
            emit loginFailed(error);
        }, Qt::SingleShotConnection);

    m_client->login(username, password);
}

void ServerConnection::registerUser(const QString& username, const QString& password)
{
    QObject::connect(m_client, &MatrixClient::registerSuccess, this,
        [this](const bsfchat::LoginResponse& resp) {
            m_userId = QString::fromStdString(resp.user_id);
            m_accessToken = QString::fromStdString(resp.access_token);
            m_deviceId = QString::fromStdString(resp.device_id);
            m_displayName = m_userId;
            m_client->setAccessToken(m_accessToken);
            m_connected = true;
            m_connectionStatus = 1;
            emit userIdChanged();
            emit displayNameChanged();
            emit connectedChanged();
            emit connectionStatusChanged();
            emit registerSucceeded();
            startSync();
        }, Qt::SingleShotConnection);

    QObject::connect(m_client, &MatrixClient::registerError, this,
        [this](const QString& error) {
            emit registerFailed(error);
        }, Qt::SingleShotConnection);

    m_client->registerUser(username, password);
}

void ServerConnection::loginWithOidc(const QString& providerUrl)
{
    if (!m_identityClient) {
        m_identityClient = new IdentityClient(this);
    }

    connect(m_identityClient, &IdentityClient::loginCompleted, this,
        [this](const QString& idToken, const QString& /*accessToken*/, const QString& /*refreshToken*/) {
            // Use the id_token to authenticate with the Matrix server
            QObject::connect(m_client, &MatrixClient::loginSuccess, this,
                [this](const bsfchat::LoginResponse& resp) {
                    m_userId = QString::fromStdString(resp.user_id);
                    m_accessToken = QString::fromStdString(resp.access_token);
                    m_deviceId = QString::fromStdString(resp.device_id);
                    m_displayName = m_userId;
                    m_client->setAccessToken(m_accessToken);
                    m_connected = true;
                    m_connectionStatus = 1;
                    emit userIdChanged();
                    emit displayNameChanged();
                    emit connectedChanged();
                    emit connectionStatusChanged();
                    emit loginSucceeded();
                    startSync();
                }, Qt::SingleShotConnection);

            QObject::connect(m_client, &MatrixClient::loginError, this,
                [this](const QString& error) {
                    emit loginFailed(error);
                }, Qt::SingleShotConnection);

            m_client->loginWithToken(idToken);
        }, Qt::SingleShotConnection);

    connect(m_identityClient, &IdentityClient::loginFailed, this,
        [this](const QString& error) {
            emit loginFailed(error);
        }, Qt::SingleShotConnection);

    m_identityClient->startLogin(providerUrl);
}

void ServerConnection::disconnectFromServer()
{
    m_syncLoop->stop();
    m_connected = false;
    m_connectionStatus = 0;
    emit connectedChanged();
    emit connectionStatusChanged();
}

void ServerConnection::startSync()
{
    m_syncLoop->start();
}

void ServerConnection::loadMembersForRoom(const QString& roomId)
{
    m_memberListModel->clear();

    // First, load from our cache (events we've seen in sync)
    if (m_roomMembers.contains(roomId)) {
        // Replay cached member events to build current state
        // We need to build the final state, not just replay — a user might have
        // joined then left. Process all events in order, keeping latest per-user.
        QMap<QString, bsfchat::RoomEvent> latestByUser;
        for (const auto& ev : m_roomMembers[roomId]) {
            if (ev.state_key.has_value()) {
                latestByUser[QString::fromStdString(*ev.state_key)] = ev;
            }
        }
        for (const auto& ev : latestByUser) {
            m_memberListModel->processEvent(ev);
        }
    }

    // Then fetch from server for completeness (handles members from before our sync)
    m_client->getRoomMembers(roomId);
}

void ServerConnection::setActiveRoom(const QString& roomId)
{
    if (m_activeRoomId == roomId) return;

    m_activeRoomId = roomId;
    m_messageModel->clear();
    m_memberListModel->clear();

    // Clear typing state when switching rooms
    if (!m_typingDisplay.isEmpty()) {
        m_typingDisplay.clear();
        m_typingUsers.clear();
        emit typingDisplayChanged();
    }
    m_typingTimer->stop();
    m_typingStopTimer->stop();

    // Get room name and topic from the room list model
    m_activeRoomName = m_roomListModel->roomDisplayName(roomId);
    m_activeRoomTopic = m_roomListModel->roomTopic(roomId);

    // Reset unread count for this room immediately (optimistic) and tell the
    // server to advance the read marker so future syncs also report zero.
    m_roomListModel->resetUnreadCount(roomId);
    if (!roomId.isEmpty()) {
        m_client->sendReadMarker(roomId);
    }

    emit activeRoomIdChanged();
    emit activeRoomNameChanged();
    emit activeRoomTopicChanged();

    // Load members and messages for this room
    if (!roomId.isEmpty()) {
        loadMembersForRoom(roomId);
        m_client->getRoomMessages(roomId, QString(), "b", 50);
    }

    // Recalculate hasUnread
    bool hadUnread = m_hasUnread;
    m_hasUnread = m_roomListModel->totalUnreadCount() > 0;
    if (m_hasUnread != hadUnread) emit hasUnreadChanged();
}

void ServerConnection::sendMessage(const QString& body)
{
    if (m_activeRoomId.isEmpty() || body.trimmed().isEmpty()) return;

    // Stop typing indicator when sending a message
    m_typingTimer->stop();
    m_typingStopTimer->stop();
    m_client->setTyping(m_activeRoomId, m_userId, false);

    m_client->sendMessage(m_activeRoomId, body);
}

void ServerConnection::sendMediaMessage(const QString& fileUrl)
{
    if (m_activeRoomId.isEmpty()) {
        emit mediaSendFailed("No active room");
        return;
    }

    // Convert QML URL to local file path
    QUrl url(fileUrl);
    QString filePath = url.isLocalFile() ? url.toLocalFile() : fileUrl;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit mediaSendFailed("Cannot open file: " + filePath);
        return;
    }

    QByteArray fileData = file.readAll();
    file.close();

    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();
    qint64 fileSize = fileInfo.size();

    // Determine MIME type
    QMimeDatabase mimeDb;
    QString mimeType = mimeDb.mimeTypeForFile(filePath).name();

    // Determine msgtype based on MIME
    QString msgtype = "m.file";
    if (mimeType.startsWith("image/")) {
        msgtype = "m.image";
    } else if (mimeType.startsWith("audio/")) {
        msgtype = "m.audio";
    } else if (mimeType.startsWith("video/")) {
        msgtype = "m.video";
    }

    // Capture state for the lambda chain
    QString roomId = m_activeRoomId;

    // Upload then send
    auto conn = QObject::connect(m_client, &MatrixClient::mediaUploaded, this,
        [this, roomId, fileName, fileSize, mimeType, msgtype](const QString& contentUri) {
            // Build the message event content
            nlohmann::json content;
            content["msgtype"] = msgtype.toStdString();
            content["body"] = fileName.toStdString();
            content["url"] = contentUri.toStdString();
            content["info"] = {
                {"mimetype", mimeType.toStdString()},
                {"size", fileSize}
            };

            QByteArray body = QByteArray::fromStdString(content.dump());
            m_client->sendRoomEvent(roomId, "m.room.message", body);
            emit mediaSendCompleted();
        }, Qt::SingleShotConnection);

    auto errConn = QObject::connect(m_client, &MatrixClient::mediaUploadError, this,
        [this](const QString& error) {
            emit mediaSendFailed(error);
        }, Qt::SingleShotConnection);

    m_client->uploadMedia(fileData, mimeType, fileName);
}

void ServerConnection::createRoom(const QString& name, const QString& topic)
{
    m_client->createRoom(name, topic);
}

void ServerConnection::joinRoom(const QString& roomIdOrAlias)
{
    m_client->joinRoom(roomIdOrAlias);
}

void ServerConnection::leaveRoom(const QString& roomId)
{
    m_client->leaveRoom(roomId);
}

void ServerConnection::resetUnreadForRoom(const QString& roomId)
{
    m_roomListModel->resetUnreadCount(roomId);
    bool hadUnread = m_hasUnread;
    m_hasUnread = m_roomListModel->totalUnreadCount() > 0;
    if (m_hasUnread != hadUnread) emit hasUnreadChanged();
}

void ServerConnection::joinVoiceChannel(const QString& roomId)
{
    // If already in a voice channel, leave it first
    if (!m_activeVoiceRoomId.isEmpty() && m_activeVoiceRoomId != roomId) {
        m_client->leaveVoice(m_activeVoiceRoomId);
        m_activeVoiceRoomId.clear();
        m_voiceMembers = QJsonArray();
        m_voicePollTimer->stop();
        emit activeVoiceRoomIdChanged();
        emit voiceMembersChanged();
    }
    m_client->joinVoice(roomId);
}

void ServerConnection::leaveVoiceChannel()
{
    if (m_activeVoiceRoomId.isEmpty()) return;
#ifdef BSFCHAT_VOICE_ENABLED
    if (m_voiceEngine) {
        m_voiceEngine->stop();
        delete m_voiceEngine;
        m_voiceEngine = nullptr;
    }
#endif
    m_client->leaveVoice(m_activeVoiceRoomId);
}

void ServerConnection::toggleMute()
{
    m_voiceMuted = !m_voiceMuted;
#ifdef BSFCHAT_VOICE_ENABLED
    m_sounds->playMute();
#endif
#ifdef BSFCHAT_VOICE_ENABLED
    if (m_voiceEngine) m_voiceEngine->setMuted(m_voiceMuted);
#endif
    emit voiceMutedChanged();
    if (!m_activeVoiceRoomId.isEmpty()) {
        m_client->updateVoiceState(m_activeVoiceRoomId, m_voiceMuted, m_voiceDeafened);
    }
}

void ServerConnection::toggleDeafen()
{
    m_voiceDeafened = !m_voiceDeafened;
#ifdef BSFCHAT_VOICE_ENABLED
    if (m_voiceEngine) m_voiceEngine->setDeafened(m_voiceDeafened);
#endif
    emit voiceDeafenedChanged();
    if (!m_activeVoiceRoomId.isEmpty()) {
        m_client->updateVoiceState(m_activeVoiceRoomId, m_voiceMuted, m_voiceDeafened);
    }
}

void ServerConnection::createVoiceChannel(const QString& name)
{
    m_client->createVoiceChannel(name);
}

void ServerConnection::sendTypingNotification()
{
    if (m_activeRoomId.isEmpty() || m_userId.isEmpty()) return;

    // Debounce: only send typing=true if the timer is not already running
    if (!m_typingTimer->isActive()) {
        m_client->setTyping(m_activeRoomId, m_userId, true, 5000);
        m_typingTimer->start();
    }

    // Reset the stop timer — user is still typing
    m_typingStopTimer->start();
}

void ServerConnection::createCategory(const QString& name)
{
    m_client->createCategoryRoom(name);
}

void ServerConnection::createChannelInCategory(const QString& name, const QString& categoryId, bool isVoice)
{
    m_client->createChannelInCategory(name, categoryId, isVoice);
}

void ServerConnection::moveChannelToCategory(const QString& roomId, const QString& categoryId)
{
    m_client->moveChannel(roomId, categoryId);
}

void ServerConnection::setChannelOrder(const QString& roomId, int order)
{
    m_client->setChannelOrder(roomId, order);
}

void ServerConnection::updateUserPowerLevel(const QString& roomId, const QString& userId, int level)
{
    // Fetch current power_levels, modify, PUT back
    auto* conn = new QMetaObject::Connection;
    *conn = connect(m_client, &MatrixClient::roomStateResult, this,
        [this, conn, roomId, userId, level](const QString& rId, const QString& evType, const QJsonObject& content) {
            if (rId != roomId || evType != QStringLiteral("m.room.power_levels")) return;
            disconnect(*conn);
            delete conn;

            QJsonObject modified = content;
            QJsonObject users = modified.value("users").toObject();
            users[userId] = level;
            modified["users"] = users;

            QByteArray body = QJsonDocument(modified).toJson(QJsonDocument::Compact);
            m_client->setRoomState(roomId, QStringLiteral("m.room.power_levels"), QString(), body);
        });

    m_client->getRoomState(roomId, QStringLiteral("m.room.power_levels"), QString());
}

void ServerConnection::updateServerRoles(const QJsonArray& rolesJson)
{
    // Find the first joined room to use as the "server room" for storing roles
    // Use the active room or fall back to any room
    QString targetRoom = m_activeRoomId;
    if (targetRoom.isEmpty() && m_roomListModel->rowCount() > 0) {
        targetRoom = m_roomListModel->data(m_roomListModel->index(0), RoomListModel::RoomIdRole).toString();
    }
    if (targetRoom.isEmpty()) return;

    QJsonObject content;
    content[QStringLiteral("roles")] = rolesJson;
    QByteArray body = QJsonDocument(content).toJson(QJsonDocument::Compact);
    m_client->setRoomState(targetRoom, QString::fromUtf8(bsfchat::event_type::kServerRoles), QString(), body);
}

void ServerConnection::rebuildCategorizedRooms()
{
    m_categorizedRooms = m_roomListModel->getCategoriesWithChannels();
    emit categorizedRoomsChanged();
}

void ServerConnection::updateDisplayName(const QString& name)
{
    if (m_userId.isEmpty()) return;
    m_client->setDisplayName(m_userId, name);
}

void ServerConnection::updateAvatarUrl(const QString& url)
{
    if (m_userId.isEmpty()) return;
    m_client->setAvatarUrl(m_userId, url);
}

void ServerConnection::uploadAvatar(const QString& fileUrl)
{
    if (m_userId.isEmpty()) return;

    QUrl url(fileUrl);
    QString filePath = url.isLocalFile() ? url.toLocalFile() : fileUrl;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return;

    QByteArray fileData = file.readAll();
    file.close();

    QMimeDatabase mimeDb;
    QString mimeType = mimeDb.mimeTypeForFile(filePath).name();
    QFileInfo fileInfo(filePath);

    QObject::connect(m_client, &MatrixClient::mediaUploaded, this,
        [this](const QString& contentUri) {
            updateAvatarUrl(contentUri);
        }, Qt::SingleShotConnection);

    m_client->uploadMedia(fileData, mimeType, fileInfo.fileName());
}

void ServerConnection::fetchProfile(const QString& userId)
{
    m_client->getProfile(userId);
}

QString ServerConnection::resolveMediaUrl(const QString& mxcUri) const
{
    return m_client->mediaDownloadUrl(mxcUri);
}

void ServerConnection::processSyncResponse(const bsfchat::SyncResponse& response)
{
    // Clear sync error on successful sync
    if (m_connectionStatus != 1) {
        m_connectionStatus = 1;
        m_syncErrorMessage.clear();
        emit connectionStatusChanged();
        emit syncErrorMessageChanged();
    }

    for (const auto& [roomIdStr, joinedRoom] : response.rooms.join) {
        QString roomId = QString::fromStdString(roomIdStr);

        // Process state events (room name, topic, members)
        for (const auto& event : joinedRoom.state.events) {
            QString type = QString::fromStdString(event.type);
            if (type == QString::fromUtf8(bsfchat::event_type::kRoomName)) {
                QString name = QString::fromStdString(event.content.data.value("name", ""));
                m_roomListModel->updateRoomName(roomId, name);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomTopic)) {
                QString topic = QString::fromStdString(event.content.data.value("topic", ""));
                m_roomListModel->updateRoomTopic(roomId, topic);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomMember)) {
                // Always cache member events for all rooms
                m_roomMembers[roomId].append(event);
                if (roomId == m_activeRoomId) {
                    m_memberListModel->processEvent(event);
                }
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomVoice)) {
                m_roomListModel->updateVoiceState(roomId, true);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kCallMember)) {
                m_client->getVoiceMembers(roomId);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomType)) {
                QString rtype = QString::fromStdString(event.content.data.value("type", ""));
                m_roomListModel->updateRoomType(roomId, rtype);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomCategory)) {
                QString parentId = QString::fromStdString(event.content.data.value("parent_id", ""));
                m_roomListModel->updateParentId(roomId, parentId);
                int order = 0;
                if (event.content.data.contains("order")) {
                    order = event.content.data["order"].get<int>();
                }
                m_roomListModel->updateSortOrder(roomId, order);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kServerRoles)) {
                if (event.content.data.contains("roles")) {
                    QByteArray rolesData = QByteArray::fromStdString(
                        nlohmann::json(event.content.data["roles"]).dump());
                    auto doc = QJsonDocument::fromJson(rolesData);
                    if (doc.isArray()) {
                        m_serverRoles = doc.array();
                        emit serverRolesChanged();
                    }
                }
            } else if (type == QStringLiteral("m.room.power_levels")) {
                if (event.content.data.contains("users")) {
                    std::string myId = m_userId.toStdString();
                    auto& users = event.content.data["users"];
                    if (users.contains(myId)) {
                        int level = users[myId].get<int>();
                        if (level != m_myPowerLevel) {
                            m_myPowerLevel = level;
                            emit myPowerLevelChanged();
                        }
                    }
                }
            }
        }

        // Ensure room exists in the list
        m_roomListModel->ensureRoom(roomId);

        // Track whether any new message events arrived for the active room,
        // so we can bump the server's read marker forward without waiting for
        // the next time the user clicks the channel.
        bool activeRoomHadNewMessage = false;

        // Process timeline events
        for (const auto& event : joinedRoom.timeline.events) {
            QString type = QString::fromStdString(event.type);

            // State events in timeline
            if (type == QString::fromUtf8(bsfchat::event_type::kRoomName)) {
                QString name = QString::fromStdString(event.content.data.value("name", ""));
                m_roomListModel->updateRoomName(roomId, name);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomMember)) {
                m_roomMembers[roomId].append(event);
                if (roomId == m_activeRoomId) {
                    m_memberListModel->processEvent(event);
                }
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomVoice)) {
                m_roomListModel->updateVoiceState(roomId, true);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kCallMember)) {
                m_client->getVoiceMembers(roomId);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomType)) {
                QString rtype = QString::fromStdString(event.content.data.value("type", ""));
                m_roomListModel->updateRoomType(roomId, rtype);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kRoomCategory)) {
                QString parentId = QString::fromStdString(event.content.data.value("parent_id", ""));
                m_roomListModel->updateParentId(roomId, parentId);
                int order = 0;
                if (event.content.data.contains("order")) {
                    order = event.content.data["order"].get<int>();
                }
                m_roomListModel->updateSortOrder(roomId, order);
            } else if (type == QString::fromUtf8(bsfchat::event_type::kServerRoles)) {
                if (event.content.data.contains("roles")) {
                    QByteArray rolesData = QByteArray::fromStdString(
                        nlohmann::json(event.content.data["roles"]).dump());
                    auto doc = QJsonDocument::fromJson(rolesData);
                    if (doc.isArray()) {
                        m_serverRoles = doc.array();
                        emit serverRolesChanged();
                    }
                }
            } else if (type == QStringLiteral("m.room.power_levels")) {
                if (event.content.data.contains("users")) {
                    std::string myId = m_userId.toStdString();
                    auto& users = event.content.data["users"];
                    if (users.contains(myId)) {
                        int level = users[myId].get<int>();
                        if (level != m_myPowerLevel) {
                            m_myPowerLevel = level;
                            emit myPowerLevelChanged();
                        }
                    }
                }
            }

            // Update last message for room list
            if (type == QString::fromUtf8(bsfchat::event_type::kRoomMessage)) {
                QString body = QString::fromStdString(event.content.data.value("body", ""));
                m_roomListModel->updateLastMessage(roomId, body, event.origin_server_ts);

                QString sender = QString::fromStdString(event.sender);
                if (roomId == m_activeRoomId && sender != m_userId) {
                    activeRoomHadNewMessage = true;
                }
            }

#ifdef BSFCHAT_VOICE_ENABLED
            // Route call signaling events to VoiceEngine
            if (roomId == m_activeVoiceRoomId && m_voiceEngine) {
                QString sender = QString::fromStdString(event.sender);
                if (sender != m_userId) {
                    const auto& c = event.content.data;
                    if (type == QString::fromUtf8(bsfchat::event_type::kCallInvite)) {
                        m_voiceEngine->handleCallInvite(sender,
                            QString::fromStdString(c.value("call_id", "")),
                            c.value("offer", nlohmann::json::object()).value("sdp", ""));
                    } else if (type == QString::fromUtf8(bsfchat::event_type::kCallAnswer)) {
                        m_voiceEngine->handleCallAnswer(sender,
                            QString::fromStdString(c.value("call_id", "")),
                            c.value("answer", nlohmann::json::object()).value("sdp", ""));
                    } else if (type == QString::fromUtf8(bsfchat::event_type::kCallCandidates)) {
                        std::vector<std::pair<std::string, std::string>> cands;
                        for (const auto& ic : c.value("candidates", nlohmann::json::array())) {
                            cands.emplace_back(ic.value("candidate", ""), ic.value("sdpMid", ""));
                        }
                        m_voiceEngine->handleCallCandidates(sender,
                            QString::fromStdString(c.value("call_id", "")), cands);
                    } else if (type == QString::fromUtf8(bsfchat::event_type::kCallHangup)) {
                        m_voiceEngine->handleCallHangup(sender,
                            QString::fromStdString(c.value("call_id", "")));
                    }
                }
            }
#endif

            // Add messages to active room's message model
            if (roomId == m_activeRoomId) {
                m_messageModel->appendEvent(event, m_userId);
            }
        }

        // Apply server-authoritative unread count. The server already accounts
        // for the read marker, sender-is-self filtering, and backfill — so we
        // just mirror whatever it tells us.
        int serverUnread = joinedRoom.unread_count.value_or(0);
        // If this is the active room, we've effectively read everything that
        // just arrived — stay at zero locally and bump the server's marker.
        if (roomId == m_activeRoomId) {
            m_roomListModel->setUnreadCount(roomId, 0);
            if (activeRoomHadNewMessage) {
                m_client->sendReadMarker(roomId);
            }
        } else {
            m_roomListModel->setUnreadCount(roomId, serverUnread);
        }

        // Process ephemeral events (typing indicators)
        if (roomId == m_activeRoomId && joinedRoom.ephemeral.has_value()) {
            for (const auto& event : joinedRoom.ephemeral->events) {
                if (event.type == std::string(bsfchat::event_type::kTyping)) {
                    QStringList typingUsers;
                    if (event.content.data.contains("user_ids")) {
                        for (const auto& uid : event.content.data["user_ids"]) {
                            QString usrId = QString::fromStdString(uid.get<std::string>());
                            // Filter out self
                            if (usrId != m_userId) {
                                // Try to get display name from member list
                                QString dn = m_memberListModel->displayNameForUser(usrId);
                                if (dn.isEmpty()) {
                                    // Fallback: extract localpart from @user:server
                                    dn = usrId;
                                    if (dn.startsWith('@')) {
                                        int colon = dn.indexOf(':');
                                        if (colon > 0) {
                                            dn = dn.mid(1, colon - 1);
                                        }
                                    }
                                }
                                typingUsers.append(dn);
                            }
                        }
                    }

                    // Build display string
                    QString display;
                    if (typingUsers.size() == 1) {
                        display = typingUsers[0] + " is typing...";
                    } else if (typingUsers.size() == 2) {
                        display = typingUsers[0] + " and " + typingUsers[1] + " are typing...";
                    } else if (typingUsers.size() > 2) {
                        display = "Several people are typing...";
                    }

                    if (display != m_typingDisplay) {
                        m_typingDisplay = display;
                        m_typingUsers = typingUsers;
                        emit typingDisplayChanged();
                    }
                }
            }
        }
    }

    // Clear typing display if active room had no ephemeral typing event
    if (!m_activeRoomId.isEmpty() && !m_typingDisplay.isEmpty()) {
        auto it = response.rooms.join.find(m_activeRoomId.toStdString());
        if (it != response.rooms.join.end()) {
            bool hadTyping = false;
            if (it->second.ephemeral.has_value()) {
                for (const auto& ev : it->second.ephemeral->events) {
                    if (ev.type == std::string(bsfchat::event_type::kTyping)) {
                        hadTyping = true;
                        break;
                    }
                }
            }
            if (!hadTyping) {
                m_typingDisplay.clear();
                m_typingUsers.clear();
                emit typingDisplayChanged();
            }
        }
    }

    // Update active room name/topic after processing
    if (!m_activeRoomId.isEmpty()) {
        QString newName = m_roomListModel->roomDisplayName(m_activeRoomId);
        if (newName != m_activeRoomName) {
            m_activeRoomName = newName;
            emit activeRoomNameChanged();
        }
        QString newTopic = m_roomListModel->roomTopic(m_activeRoomId);
        if (newTopic != m_activeRoomTopic) {
            m_activeRoomTopic = newTopic;
            emit activeRoomTopicChanged();
        }
    }

    // Rebuild categorized rooms after sync processing
    rebuildCategorizedRooms();

    // Recalculate global hasUnread flag from the authoritative per-room counts.
    bool hadUnread = m_hasUnread;
    m_hasUnread = m_roomListModel->totalUnreadCount() > 0;
    if (m_hasUnread != hadUnread) emit hasUnreadChanged();
}
