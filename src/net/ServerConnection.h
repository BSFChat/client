#pragma once

#include <QJsonArray>
#include <QObject>
#include <QVariantList>
#include <QString>
#include <QTimer>

#include <bsfchat/MatrixTypes.h>

class MatrixClient;
class SyncLoop;
class RoomListModel;
class MessageModel;
class MemberListModel;
#ifdef BSFCHAT_VOICE_ENABLED
class VoiceEngine;
class NotificationSounds;
#endif
class IdentityClient;

class ServerConnection : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString displayName READ displayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY avatarUrlChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    Q_PROPERTY(QString userId READ userId NOTIFY userIdChanged)
    Q_PROPERTY(RoomListModel* roomListModel READ roomListModel CONSTANT)
    Q_PROPERTY(MessageModel* messageModel READ messageModel CONSTANT)
    Q_PROPERTY(MemberListModel* memberListModel READ memberListModel CONSTANT)
    Q_PROPERTY(QString activeRoomId READ activeRoomId NOTIFY activeRoomIdChanged)
    Q_PROPERTY(QString activeRoomName READ activeRoomName NOTIFY activeRoomNameChanged)
    Q_PROPERTY(QString activeRoomTopic READ activeRoomTopic NOTIFY activeRoomTopicChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(int connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncErrorMessage READ syncErrorMessage NOTIFY syncErrorMessageChanged)
    Q_PROPERTY(bool hasUnread READ hasUnread NOTIFY hasUnreadChanged)
    Q_PROPERTY(QString activeVoiceRoomId READ activeVoiceRoomId NOTIFY activeVoiceRoomIdChanged)
    Q_PROPERTY(bool voiceMuted READ voiceMuted NOTIFY voiceMutedChanged)
    Q_PROPERTY(bool voiceDeafened READ voiceDeafened NOTIFY voiceDeafenedChanged)
    Q_PROPERTY(bool inVoiceChannel READ inVoiceChannel NOTIFY activeVoiceRoomIdChanged)
    Q_PROPERTY(QJsonArray voiceMembers READ voiceMembers NOTIFY voiceMembersChanged)
    Q_PROPERTY(QString typingDisplay READ typingDisplay NOTIFY typingDisplayChanged)
    Q_PROPERTY(int myPowerLevel READ myPowerLevel NOTIFY myPowerLevelChanged)
    Q_PROPERTY(QJsonArray serverRoles READ serverRoles NOTIFY serverRolesChanged)
    Q_PROPERTY(QVariantList categorizedRooms READ categorizedRooms NOTIFY categorizedRoomsChanged)

public:
    explicit ServerConnection(const QString& serverUrl, QObject* parent = nullptr);
    ~ServerConnection() override;

    // Properties
    QString displayName() const { return m_displayName; }
    QString avatarUrl() const { return m_avatarUrl; }
    QString serverUrl() const { return m_serverUrl; }
    QString userId() const { return m_userId; }
    QString accessToken() const { return m_accessToken; }
    QString deviceId() const { return m_deviceId; }
    bool isConnected() const { return m_connected; }
    QString activeRoomId() const { return m_activeRoomId; }
    QString activeRoomName() const { return m_activeRoomName; }
    QString activeRoomTopic() const { return m_activeRoomTopic; }

    // 0 = disconnected, 1 = connected/syncing, 2 = reconnecting
    int connectionStatus() const { return m_connectionStatus; }
    QString syncErrorMessage() const { return m_syncErrorMessage; }
    bool hasUnread() const { return m_hasUnread; }

    QString activeVoiceRoomId() const { return m_activeVoiceRoomId; }
    bool voiceMuted() const { return m_voiceMuted; }
    bool voiceDeafened() const { return m_voiceDeafened; }
    bool inVoiceChannel() const { return !m_activeVoiceRoomId.isEmpty(); }
    QJsonArray voiceMembers() const { return m_voiceMembers; }
    QString typingDisplay() const { return m_typingDisplay; }
    int myPowerLevel() const { return m_myPowerLevel; }
    QJsonArray serverRoles() const { return m_serverRoles; }
    QVariantList categorizedRooms() const { return m_categorizedRooms; }

    RoomListModel* roomListModel() const { return m_roomListModel; }
    MessageModel* messageModel() const { return m_messageModel; }
    MemberListModel* memberListModel() const { return m_memberListModel; }
    MatrixClient* client() const { return m_client; }

    // Set credentials (for restoring from settings)
    void setCredentials(const QString& userId, const QString& accessToken,
                        const QString& deviceId, const QString& displayName);

    // Actions
    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void registerUser(const QString& username, const QString& password);
    Q_INVOKABLE void disconnectFromServer();
    Q_INVOKABLE void setActiveRoom(const QString& roomId);
    Q_INVOKABLE void sendMessage(const QString& body);
    Q_INVOKABLE void sendMediaMessage(const QString& fileUrl);
    Q_INVOKABLE void createRoom(const QString& name, const QString& topic);
    Q_INVOKABLE void joinRoom(const QString& roomIdOrAlias);
    Q_INVOKABLE void leaveRoom(const QString& roomId);
    Q_INVOKABLE void resetUnreadForRoom(const QString& roomId);

    Q_INVOKABLE void loginWithOidc(const QString& providerUrl);

    Q_INVOKABLE void joinVoiceChannel(const QString& roomId);
    Q_INVOKABLE void leaveVoiceChannel();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleDeafen();
    Q_INVOKABLE void createVoiceChannel(const QString& name);

    Q_INVOKABLE void sendTypingNotification();

    // Category & channel management
    Q_INVOKABLE void createCategory(const QString& name);
    Q_INVOKABLE void createChannelInCategory(const QString& name, const QString& categoryId, bool isVoice = false);
    Q_INVOKABLE void moveChannelToCategory(const QString& roomId, const QString& categoryId);
    Q_INVOKABLE void setChannelOrder(const QString& roomId, int order);
    Q_INVOKABLE void updateUserPowerLevel(const QString& roomId, const QString& userId, int level);
    Q_INVOKABLE void updateServerRoles(const QJsonArray& rolesJson);

    Q_INVOKABLE void updateDisplayName(const QString& name);
    Q_INVOKABLE void updateAvatarUrl(const QString& url);
    Q_INVOKABLE void uploadAvatar(const QString& fileUrl);
    Q_INVOKABLE void fetchProfile(const QString& userId);
    Q_INVOKABLE QString resolveMediaUrl(const QString& mxcUri) const;

signals:
    void displayNameChanged();
    void userIdChanged();
    void activeRoomIdChanged();
    void activeRoomNameChanged();
    void connectedChanged();
    void connectionStatusChanged();
    void syncErrorMessageChanged();
    void activeRoomTopicChanged();
    void hasUnreadChanged();
    void loginSucceeded();
    void loginFailed(const QString& error);
    void registerSucceeded();
    void registerFailed(const QString& error);
    void mediaSendCompleted();
    void mediaSendFailed(const QString& error);
    void activeVoiceRoomIdChanged();
    void voiceMutedChanged();
    void voiceDeafenedChanged();
    void voiceMembersChanged();
    void avatarUrlChanged();
    void typingDisplayChanged();
    void profileFetched(const QString& userId, const QString& displayName, const QString& avatarUrl);
    void myPowerLevelChanged();
    void serverRolesChanged();
    void categorizedRoomsChanged();

private:
    void startSync();
    void processSyncResponse(const bsfchat::SyncResponse& response);

    MatrixClient* m_client;
    SyncLoop* m_syncLoop;
    RoomListModel* m_roomListModel;
    MessageModel* m_messageModel;
    MemberListModel* m_memberListModel;

    QString m_serverUrl;
    QString m_userId;
    QString m_accessToken;
    QString m_deviceId;
    QString m_displayName;
    QString m_avatarUrl;
    QString m_activeRoomId;
    QString m_activeRoomName;
    QString m_activeRoomTopic;
    bool m_connected = false;
    int m_connectionStatus = 0; // 0=disconnected, 1=connected, 2=reconnecting
    QString m_syncErrorMessage;
    bool m_hasUnread = false;

    // Per-room member cache: roomId -> list of member events
    QMap<QString, QVector<bsfchat::RoomEvent>> m_roomMembers;
    void loadMembersForRoom(const QString& roomId);

    // Identity (OIDC)
    IdentityClient* m_identityClient = nullptr;

    // Typing state
    QStringList m_typingUsers;
    QString m_typingDisplay;
    QTimer* m_typingTimer = nullptr;
    QTimer* m_typingStopTimer = nullptr;

    // Voice state
    QString m_activeVoiceRoomId;
    bool m_voiceMuted = false;
    bool m_voiceDeafened = false;
    QJsonArray m_voiceMembers;
    QTimer* m_voicePollTimer = nullptr;
#ifdef BSFCHAT_VOICE_ENABLED
    VoiceEngine* m_voiceEngine = nullptr;
    NotificationSounds* m_sounds = nullptr;
#endif

    // Category/roles state
    int m_myPowerLevel = 0;
    QJsonArray m_serverRoles;
    QVariantList m_categorizedRooms;
    void rebuildCategorizedRooms();
};
