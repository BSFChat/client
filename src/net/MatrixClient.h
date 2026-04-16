#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <bsfchat/MatrixTypes.h>

class MatrixClient : public QObject {
    Q_OBJECT

public:
    explicit MatrixClient(QObject* parent = nullptr);

    void setHomeserver(const QString& url);
    QString homeserver() const { return m_homeserver; }

    void setAccessToken(const QString& token);
    QString accessToken() const { return m_accessToken; }

    // Auth
    void login(const QString& username, const QString& password);
    void loginWithToken(const QString& idToken);
    void getLoginFlows();
    void registerUser(const QString& username, const QString& password);

    // Sync
    void sync(const QString& since = {}, int timeout = 30000);

    // Rooms
    void createRoom(const QString& name, const QString& topic, const QString& visibility = "private");
    void joinRoom(const QString& roomIdOrAlias);
    void leaveRoom(const QString& roomId);
    void deleteRoom(const QString& roomId);
    void getJoinedRooms();
    void getRoomMembers(const QString& roomId);

    // Messages
    void sendMessage(const QString& roomId, const QString& body);
    void sendRoomEvent(const QString& roomId, const QString& eventType, const QByteArray& content);
    void getRoomMessages(const QString& roomId, const QString& from, const QString& dir = "b", int limit = 50);

    // Media
    void uploadMedia(const QByteArray& data, const QString& contentType, const QString& filename);
    QString mediaDownloadUrl(const QString& mxcUri) const;

    // Profile
    void getProfile(const QString& userId);
    void setDisplayName(const QString& userId, const QString& displayName);
    void setAvatarUrl(const QString& userId, const QString& avatarUrl);

    // Typing
    void setTyping(const QString& roomId, const QString& userId, bool typing, int timeout = 5000);

    // Read marker (server-tracked unread counts).
    // Server marks everything currently in the room as read for this user.
    void sendReadMarker(const QString& roomId);

    // Permissions / roles
    void setMemberRoles(const QString& roomId, const QString& userId, const QStringList& roleIds);
    // targetKey is "role:<id>" or "user:<mxid>". Pass allow=0,deny=0 to clear.
    void setChannelPermission(const QString& roomId, const QString& targetKey,
                              quint64 allow, quint64 deny);
    void setChannelSlowmode(const QString& roomId, int seconds);
    void redactEvent(const QString& roomId, const QString& eventId, const QString& reason = {});
    void kickUser(const QString& roomId, const QString& userId, const QString& reason = {});
    void banUser(const QString& roomId, const QString& userId, const QString& reason = {});

    // Voice
    void joinVoice(const QString& roomId);
    void leaveVoice(const QString& roomId);
    void getVoiceMembers(const QString& roomId);
    void updateVoiceState(const QString& roomId, bool muted, bool deafened);
    void createVoiceChannel(const QString& name);
    void getTurnConfig();

    // Categories & Channels
    void createCategoryRoom(const QString& name);
    // Create a text or voice channel. Channels are always created public so
    // they auto-join to everyone — privacy is later enforced by a per-channel
    // @everyone DENY VIEW_CHANNEL override, applied separately by the caller
    // listening on createRoomSuccess.
    void createChannelInCategory(const QString& name, const QString& categoryId, bool isVoice = false);
    void moveChannel(const QString& roomId, const QString& categoryId);
    void setChannelOrder(const QString& roomId, int order);
    void setRoomState(const QString& roomId, const QString& eventType, const QString& stateKey, const QByteArray& content);
    void getRoomState(const QString& roomId, const QString& eventType, const QString& stateKey);

signals:
    void loginSuccess(const bsfchat::LoginResponse& response);
    void loginError(const QString& error);

    void registerSuccess(const bsfchat::LoginResponse& response);
    void registerError(const QString& error);

    void syncSuccess(const bsfchat::SyncResponse& response);
    void syncError(const QString& error);

    void createRoomSuccess(const QString& roomId);
    void createRoomError(const QString& error);

    void joinRoomSuccess(const QString& roomId);
    void joinRoomError(const QString& error);

    void leaveRoomSuccess(const QString& roomId);
    void leaveRoomError(const QString& error);

    void joinedRoomsResult(const QStringList& roomIds);
    void roomMembersResult(const QString& roomId, const QJsonArray& members);

    void messageSent(const QString& eventId);
    void sendMessageError(const QString& error);

    void messagesResult(const bsfchat::MessagesResponse& response);
    void messagesError(const QString& error);

    void mediaUploaded(const QString& contentUri);
    void mediaUploadError(const QString& error);

    void voiceJoined(const QString& roomId, const QJsonArray& members);
    void voiceLeft(const QString& roomId);
    void voiceMembersResult(const QString& roomId, const QJsonArray& members);
    void voiceError(const QString& error);
    void voiceChannelCreated(const QString& roomId);

    void turnConfigResult(const QJsonObject& config);

    void profileResult(const QString& userId, const QString& displayName, const QString& avatarUrl);

    void categoryRoomCreated(const QString& roomId);
    void channelMoved();
    void channelOrderSet();
    void roomStateResult(const QString& roomId, const QString& eventType, const QJsonObject& content);
    void displayNameUpdated();
    void avatarUrlUpdated();

    void loginFlowsResult(const QJsonArray& flows);

private:
    QNetworkReply* makeRequest(const QString& method, const QString& path,
                                const QByteArray& body = {});
    QUrl buildUrl(const QString& path) const;

    QNetworkAccessManager m_nam;
    QString m_homeserver;
    QString m_accessToken;
};
