#pragma once

#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QString>
#include <QTimer>
#include <QVector>

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
    // Human-readable name of the BSFChat server (e.g. "BSFChat"). Set
    // server-side via a bsfchat.server.info state event, gated on
    // MANAGE_SERVER. Falls back to the server's host if unset.
    Q_PROPERTY(QString serverName READ serverName NOTIFY serverNameChanged)
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
    // Mic transmit level, 0..1. Non-zero when the mic is open AND capturing
    // audio above the silence floor; zero when muted, disconnected, or idle.
    Q_PROPERTY(float micLevel READ micLevel NOTIFY micLevelChanged)
    Q_PROPERTY(bool micSilent READ micSilent NOTIFY micSilentChanged)
    Q_PROPERTY(QString typingDisplay READ typingDisplay NOTIFY typingDisplayChanged)
    Q_PROPERTY(int myPowerLevel READ myPowerLevel NOTIFY myPowerLevelChanged)
    Q_PROPERTY(QJsonArray serverRoles READ serverRoles NOTIFY serverRolesChanged)
    Q_PROPERTY(QVariantList categorizedRooms READ categorizedRooms NOTIFY categorizedRoomsChanged)
    // Monotonically increases whenever any permission-relevant state changes
    // (server roles, member roles, channel overrides, channel settings).
    // QML bindings should reference this so they re-evaluate whenever the
    // effective permission set could have changed.
    Q_PROPERTY(int permissionsGeneration READ permissionsGeneration NOTIFY permissionsChanged)

public:
    explicit ServerConnection(const QString& serverUrl, QObject* parent = nullptr);
    ~ServerConnection() override;

    // Properties
    QString displayName() const { return m_displayName; }
    QString avatarUrl() const { return m_avatarUrl; }
    QString serverUrl() const { return m_serverUrl; }
    QString serverName() const;
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
    // Returns the voice-member list with each member augmented by a
    // "peerState" key ("connected"/"connecting"/"failed"/etc.) so the
    // VoicePanel can show per-peer indicators.
    QJsonArray voiceMembers() const;
    float micLevel() const { return m_micLevel; }
    // True when AudioEngine has been capturing silence for a sustained
    // period — signals a device/permission problem the user should see.
    bool micSilent() const { return m_micSilent; }
    QString typingDisplay() const { return m_typingDisplay; }
    int myPowerLevel() const { return m_myPowerLevel; }
    QJsonArray serverRoles() const { return m_serverRoles; }
    QVariantList categorizedRooms() const { return m_categorizedRooms; }
    int permissionsGeneration() const { return m_permissionsGeneration; }

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
    // Edit a previously-sent message in the currently-active room. Server
    // rejects if the caller isn't the original sender.
    Q_INVOKABLE void editMessage(const QString& eventId, const QString& newBody);
    Q_INVOKABLE void sendMediaMessage(const QString& fileUrl);
    Q_INVOKABLE void createRoom(const QString& name, const QString& topic);
    Q_INVOKABLE void joinRoom(const QString& roomIdOrAlias);
    Q_INVOKABLE void leaveRoom(const QString& roomId);
    // Delete a channel (server-side destructive). Requires MANAGE_CHANNELS;
    // server enforces regardless.
    Q_INVOKABLE void deleteChannel(const QString& roomId);
    Q_INVOKABLE void resetUnreadForRoom(const QString& roomId);

    Q_INVOKABLE void loginWithOidc(const QString& providerUrl);
    // Returns the identity-provider base URL (e.g. "https://id.bsfchat.com")
    // for the "Manage Account" link. Empty if OIDC was never used.
    Q_INVOKABLE QString identityProviderUrl() const;

    Q_INVOKABLE void joinVoiceChannel(const QString& roomId);
    Q_INVOKABLE void leaveVoiceChannel();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleDeafen();
    Q_INVOKABLE void createVoiceChannel(const QString& name);

    Q_INVOKABLE void sendTypingNotification();

    // Category & channel management
    Q_INVOKABLE void createCategory(const QString& name);
    Q_INVOKABLE void createChannelInCategory(const QString& name, const QString& categoryId, bool isVoice = false, bool makePrivate = false);
    Q_INVOKABLE void moveChannelToCategory(const QString& roomId, const QString& categoryId);
    Q_INVOKABLE void setChannelOrder(const QString& roomId, int order);
    Q_INVOKABLE void updateUserPowerLevel(const QString& roomId, const QString& userId, int level);
    Q_INVOKABLE void updateServerRoles(const QJsonArray& rolesJson);

    // Discord-style permissions API surface.
    // Returns the effective permission bitfield for the given room, computed
    // client-side using the same algorithm the server uses. For gating UI
    // decisions; the server still enforces on every request.
    Q_INVOKABLE quint64 myPermissions(const QString& roomId) const;
    Q_INVOKABLE bool canSend(const QString& roomId) const;
    Q_INVOKABLE bool canAttach(const QString& roomId) const;
    Q_INVOKABLE bool canEmbed(const QString& roomId) const;
    Q_INVOKABLE bool canManageChannel(const QString& roomId) const;
    Q_INVOKABLE bool canManageRoles(const QString& roomId) const;
    Q_INVOKABLE bool canKick(const QString& roomId) const;
    Q_INVOKABLE bool canBan(const QString& roomId) const;
    Q_INVOKABLE bool canManageMessages(const QString& roomId) const;
    Q_INVOKABLE int channelSlowmode(const QString& roomId) const;

    // Assign/unassign roles for a user (absolute list). Server-side requires MANAGE_ROLES.
    Q_INVOKABLE void setMemberRoles(const QString& userId, const QStringList& roleIds);
    // Returns the role IDs currently assigned to `userId`, empty list if none cached.
    Q_INVOKABLE QStringList memberRoles(const QString& userId) const;
    // All per-channel overrides (allow/deny hex strings keyed by "role:..."/"user:...").
    Q_INVOKABLE QVariantList channelOverrides(const QString& roomId) const;
    Q_INVOKABLE void setChannelOverride(const QString& roomId, const QString& targetKey,
                                        quint64 allow, quint64 deny);
    Q_INVOKABLE void setChannelSlowmode(const QString& roomId, int seconds);
    Q_INVOKABLE void redactEvent(const QString& roomId, const QString& eventId,
                                  const QString& reason = {});
    Q_INVOKABLE void kickMember(const QString& roomId, const QString& userId,
                                 const QString& reason = {});
    Q_INVOKABLE void banMember(const QString& roomId, const QString& userId,
                                const QString& reason = {});

    Q_INVOKABLE void updateDisplayName(const QString& name);
    // Update the server-wide name (bsfchat.server.info). Requires MANAGE_SERVER.
    Q_INVOKABLE void updateServerName(const QString& name);
    Q_INVOKABLE void updateAvatarUrl(const QString& url);
    Q_INVOKABLE void uploadAvatar(const QString& fileUrl);
    Q_INVOKABLE void fetchProfile(const QString& userId);
    Q_INVOKABLE QString resolveMediaUrl(const QString& mxcUri) const;

signals:
    void displayNameChanged();
    void serverNameChanged();
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
    void micLevelChanged();
    void micSilentChanged();
    void avatarUrlChanged();
    void typingDisplayChanged();
    void profileFetched(const QString& userId, const QString& displayName, const QString& avatarUrl);
    void myPowerLevelChanged();
    void serverRolesChanged();
    void permissionsChanged();
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
    QString m_serverName; // set by bsfchat.server.info state event
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
    // Global userId → display name, populated from all m.room.member events
    // across every room. MessageModel reads from this pointer.
    QMap<QString, QString> m_userDisplayNames;
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
    float m_micLevel = 0.0f;
    bool m_micSilent = false;
    int m_zeroLevelFrames = 0; // consecutive frames with near-zero level
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
    int m_permissionsGeneration = 0;
    QJsonArray m_serverRoles;
    QVariantList m_categorizedRooms;
    void rebuildCategorizedRooms();

    // Typed permission caches, populated from sync state events.
    struct RoleInfo {
        QString id;
        QString name;
        QString color;
        int position = 0;
        quint64 permissions = 0;
        bool mentionable = false;
        bool hoist = false;
    };
    struct Override {
        QString targetKey; // "role:..." or "user:..."
        quint64 allow = 0;
        quint64 deny = 0;
    };
    QVector<RoleInfo> m_roles;                         // server-wide
    QMap<QString, QStringList> m_memberRoles;          // userId -> role ids
    QMap<QString, QVector<Override>> m_channelOverrides; // roomId -> list
    QMap<QString, int> m_channelSlowmode;              // roomId -> seconds

    // Helper: parse a JSON state event into typed caches.
    void applyServerRolesEvent(const QJsonObject& content);
    void applyMemberRolesEvent(const QString& userId, const QJsonObject& content);
    void applyChannelPermissionsEvent(const QString& roomId, const QString& stateKey,
                                       const QJsonObject& content);
    void applyChannelSettingsEvent(const QString& roomId, const QJsonObject& content);

    // Track first sync in this session — only prune hidden rooms once, since
    // subsequent (incremental) syncs only include rooms with new activity.
    bool m_firstSyncProcessed = false;
    QSet<QString> m_knownRoomIds;
};
