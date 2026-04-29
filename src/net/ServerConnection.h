#pragma once

#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QString>
#include <QTimer>
#include <QVector>
#include <functional>

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

class Settings;

class ServerConnection : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString displayName READ displayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY avatarUrlChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    // Human-readable name of the BSFChat server (e.g. "BSFChat"). Set
    // server-side via a bsfchat.server.info state event, gated on
    // MANAGE_SERVER. Falls back to the server's host if unset.
    Q_PROPERTY(QString serverName READ serverName NOTIFY serverNameChanged)
    // Server icon — fully-resolved HTTP URL (empty if no icon is set).
    // Stored on the same bsfchat.server.info state event as the name.
    Q_PROPERTY(QString serverAvatarUrl READ serverAvatarUrl NOTIFY serverAvatarUrlChanged)
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
    // Which view the main area should display. Independent of whether
    // the user is connected to a voice call — you can be in voice AND
    // reading a text channel. Flips true when the voice channel row is
    // clicked (or the VoiceDock / VoiceStatusCard is tapped), false
    // whenever a text channel becomes active or voice is left.
    Q_PROPERTY(bool viewingVoiceRoom READ viewingVoiceRoom NOTIFY viewingVoiceRoomChanged)
    Q_PROPERTY(QJsonArray voiceMembers READ voiceMembers NOTIFY voiceMembersChanged)
    // Mic transmit level, 0..1. Non-zero when the mic is open AND capturing
    // audio above the silence floor; zero when muted, disconnected, or idle.
    Q_PROPERTY(float micLevel READ micLevel NOTIFY micLevelChanged)
    Q_PROPERTY(bool micSilent READ micSilent NOTIFY micSilentChanged)
    Q_PROPERTY(QString typingDisplay READ typingDisplay NOTIFY typingDisplayChanged)
    // Monotonic counter bumped whenever per-room typing state changes.
    // QML bindings read this as a dependency then call
    // `roomHasTyping(roomId)` to drive the sidebar typing indicators.
    Q_PROPERTY(int typingGeneration READ typingGeneration NOTIFY roomTypingChanged)
    Q_PROPERTY(int myPowerLevel READ myPowerLevel NOTIFY myPowerLevelChanged)
    Q_PROPERTY(QJsonArray serverRoles READ serverRoles NOTIFY serverRolesChanged)
    Q_PROPERTY(QVariantList categorizedRooms READ categorizedRooms NOTIFY categorizedRoomsChanged)
    // Monotonically increases whenever any permission-relevant state changes
    // (server roles, member roles, channel overrides, channel settings).
    // QML bindings should reference this so they re-evaluate whenever the
    // effective permission set could have changed.
    Q_PROPERTY(int permissionsGeneration READ permissionsGeneration NOTIFY permissionsChanged)
    // Aggregated list of banned users across every room the sync has
    // surfaced. One entry per user: { userId, displayName, rooms[], reason }.
    // The value comes from m_roomMembers (client-side cache), so it only
    // covers rooms we've seen — good enough for the Bans tab UI, not a
    // source of truth.
    Q_PROPERTY(QVariantList bannedMembers READ bannedMembers NOTIFY bannedMembersChanged)
    // Union of every "join"-state member across every room we've synced.
    // Distinct from memberListModel (which tracks the currently-active
    // room only). ServerSettings > Members binds to this so the tab shows
    // the full roster regardless of which channel happens to be active.
    Q_PROPERTY(QVariantList serverMembers READ serverMembers NOTIFY serverMembersChanged)

public:
    explicit ServerConnection(const QString& serverUrl, QObject* parent = nullptr);
    ~ServerConnection() override;

    // Properties
    QString displayName() const { return m_displayName; }
    QString avatarUrl() const { return m_avatarUrl; }
    QString serverUrl() const { return m_serverUrl; }
    QString serverName() const;
    QString serverAvatarUrl() const { return m_serverAvatarUrl; }
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
#ifdef BSFCHAT_VOICE_ENABLED
    // Accessor for the screen-share controller to bind to the
    // currently-running voice engine. Null when no voice session.
    // Only present in voice-enabled builds (desktop); mobile MVP
    // ships without voice until libdatachannel is wired on iOS.
    VoiceEngine* voiceEngine() const { return m_voiceEngine; }
#endif

    // Receive side for screen share: the latest JPEG frame from each
    // remote peer, exposed as an encoded data URL the QML Image
    // element can consume directly. Empty map when nobody is sharing.
    Q_INVOKABLE QString peerScreenDataUrl(const QString& userId) const {
        auto it = m_peerScreenData.constFind(userId);
        if (it == m_peerScreenData.constEnd()) return {};
        return *it;
    }
    Q_INVOKABLE QStringList peersCurrentlySharing() const {
        return m_peerScreenData.keys();
    }
    // Same pattern for webcam — data URL per peer, keys = peers with
    // an active camera stream.
    Q_INVOKABLE QString peerCameraDataUrl(const QString& userId) const {
        auto it = m_peerCameraData.constFind(userId);
        if (it == m_peerCameraData.constEnd()) return {};
        return *it;
    }
    Q_INVOKABLE bool peerHasCamera(const QString& userId) const {
        return m_peerCameraData.contains(userId);
    }

    // 0..1 smoothed audio level for a remote peer, updated on every
    // decoded Opus frame. ParticipantTile binds its speaking ring to
    // this. Empty for unknown/silent peers.
    Q_INVOKABLE float peerLevel(const QString& userId) const {
        return m_peerLevels.value(userId, 0.0f);
    }
    // Returns the voice-member list with each member augmented by a
    // "peerState" key ("connected"/"connecting"/"failed"/etc.) so the
    // VoicePanel can show per-peer indicators.
    QJsonArray voiceMembers() const;
    float micLevel() const { return m_micLevel; }
    // True when AudioEngine has been capturing silence for a sustained
    // period — signals a device/permission problem the user should see.
    bool micSilent() const { return m_micSilent; }
    QString typingDisplay() const { return m_typingDisplay; }
    int typingGeneration() const { return m_typingGeneration; }
    Q_INVOKABLE bool roomHasTyping(const QString& roomId) const {
        return !m_roomTyping.value(roomId).isEmpty();
    }
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
    // Send an m.emote message (the /me slash command). Renders in
    // italics + "<sender> <body>" form on the receiving side.
    Q_INVOKABLE void sendEmote(const QString& body);
    // Send a threaded reply. The message carries m.relates_to with
    // rel_type=m.thread and event_id=rootEventId. Uses the active room.
    Q_INVOKABLE void sendThreadReply(const QString& rootEventId, const QString& body);
    // Rich variant used when the composer has tracked @mention or #channel
    // tokens. `formattedBody` is the sender-generated HTML (with <a>
    // anchors for mentions/channels); `mentionedUserIds` goes into
    // m.mentions.user_ids so the server can elevate notifications.
    Q_INVOKABLE void sendRichMessage(const QString& body,
                                      const QString& formattedBody,
                                      const QStringList& mentionedUserIds);
    // Edit a previously-sent message in the currently-active room. Server
    // rejects if the caller isn't the original sender.
    Q_INVOKABLE void editMessage(const QString& eventId, const QString& newBody);
    // Send `body` into the currently-active room as a reply pointing at
    // `targetEventId`. The UI composer calls this when the reply banner is
    // active; no server-side support is needed beyond arbitrary content.
    Q_INVOKABLE void replyToMessage(const QString& targetEventId, const QString& body);
    // Forward the message identified by sourceEventId (in the active room)
    // into destRoomId on this server. Builds the attribution prefix from
    // the message model + RoomListModel.
    Q_INVOKABLE void forwardMessage(const QString& sourceEventId, const QString& destRoomId);

    // Back-pagination. Issues GET /rooms/{id}/messages?from=<prev_batch>&dir=b.
    // No-op if we've already reached the start of the room or a request is
    // already in flight. MessageView binds hasMoreHistory/loadingHistory on
    // MessageModel to drive the scroll-to-top trigger.
    Q_INVOKABLE void loadOlderMessages(int limit = 50);

    // Emoji reactions (Matrix m.reaction / m.annotation). If the current
    // user already has a reaction with `emoji` on `targetEventId`, redact it
    // (un-react); otherwise send a new reaction.
    Q_INVOKABLE void toggleReaction(const QString& targetEventId, const QString& emoji);

    // Look up a text channel by its display name (case-insensitive) and
    // make it active. No-op if there's no match. Used by the
    // #channel-mention click handler.
    Q_INVOKABLE void activateRoomByName(const QString& name);

    // Build a self-contained link to a message: bsfchat://message/<server>/<room>/<event>.
    // Safe to call with any event id in the active room — we percent-encode
    // all segments. Returns "" if no active room.
    Q_INVOKABLE QString messageLink(const QString& eventId) const;
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
    // Swap the main content to the VoiceRoom view without touching the
    // voice connection itself. Called when the user clicks an already-
    // joined voice channel, the VoiceDock, or the VoiceStatusCard. No-op
    // if the user isn't in voice.
    Q_INVOKABLE void showVoiceRoom();
    // Swap back to the text view. setActiveRoom() calls this implicitly
    // so clicking any text channel also flips the view.
    Q_INVOKABLE void showTextView();
    Q_INVOKABLE void toggleMute();

    // Push-to-talk state. In PTT voice mode the mic is force-muted
    // whenever `pttPressed` is false, regardless of the user's
    // voiceMuted toggle. QML sets this from a global keyboard
    // shortcut while the key is held, or (on mobile) from a
    // hold-to-talk button in VoiceDock — which needs a reactive
    // binding, hence the Q_PROPERTY + notify signal.
    Q_PROPERTY(bool pttPressed READ pttPressed NOTIFY pttPressedChanged)
    bool pttPressed() const { return m_pttPressed; }
    Q_INVOKABLE void setPttPressed(bool pressed);
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
    // Per-channel name + topic editors. Write m.room.name / m.room.topic
    // state events; server gates on kManageChannels.
    Q_INVOKABLE void setRoomName(const QString& roomId, const QString& name);
    Q_INVOKABLE void setRoomTopic(const QString& roomId, const QString& topic);

    // Pinned messages — Matrix m.room.pinned_events. togglePinnedEvent
    // flips whether `eventId` appears in the room's pinned list. The
    // UI uses pinnedEventIds() for display; populated live from sync.
    Q_INVOKABLE QStringList pinnedEventIds(const QString& roomId) const;
    Q_INVOKABLE bool isEventPinned(const QString& roomId, const QString& eventId) const;
    Q_INVOKABLE void togglePinnedEvent(const QString& roomId, const QString& eventId);

    Q_INVOKABLE void redactEvent(const QString& roomId, const QString& eventId,
                                  const QString& reason = {});
    Q_INVOKABLE void kickMember(const QString& roomId, const QString& userId,
                                 const QString& reason = {});
    Q_INVOKABLE void banMember(const QString& roomId, const QString& userId,
                                const QString& reason = {});
    Q_INVOKABLE void unbanMember(const QString& roomId, const QString& userId);

    // Server-scope moderation — iterate every room the bot knows about and
    // apply the matching Matrix per-room action. "Server-wide ban" maps to
    // banning the user from each room; next sync updates the caches.
    Q_INVOKABLE void kickFromServer(const QString& userId, const QString& reason = {});
    Q_INVOKABLE void banFromServer(const QString& userId, const QString& reason = {});
    Q_INVOKABLE void unbanFromServer(const QString& userId);

    // Getter for the Q_PROPERTY declared above. Rebuilds from m_roomMembers
    // each call (cheap at our expected member counts). Entries:
    //   { userId, displayName, rooms: [roomId...], reason }
    QVariantList bannedMembers() const;
    // Server-wide members union. Entries:
    //   { userId, displayName, avatarUrl, rooms: [roomId...] }
    // Only users whose LATEST membership per room is "join" are included;
    // a user who's "join" in one room and "leave" in another still counts
    // as long as at least one room shows them as joined.
    QVariantList serverMembers() const;

    Q_INVOKABLE void updateDisplayName(const QString& name);
    // Update the server-wide name (bsfchat.server.info). Requires MANAGE_SERVER.
    Q_INVOKABLE void updateServerName(const QString& name);
    Q_INVOKABLE void updateAvatarUrl(const QString& url);

    // Upload a local image file as the server icon. Uploads to Matrix
    // media then writes the mxc:// URI into bsfchat.server.info's
    // `avatar` field (preserving the existing name).
    Q_INVOKABLE void uploadServerAvatar(const QString& fileUrl);

    // Presence — client-side activity heuristic until the server
    // starts emitting m.presence. Any user whose last observed
    // event timestamp falls inside the "active" window is reported
    // as "online". `self` consults the local Settings preference
    // ("online" / "idle" / "dnd" / "offline"). Returns one of the
    // above four strings.
    Q_INVOKABLE QString presenceFor(const QString& userId) const;
    Q_INVOKABLE void setSelfPresence(const QString& state);
    Q_INVOKABLE QString selfPresence() const { return m_selfPresence; }

    // Custom status message — free-form text shown under display
    // names. Persisted server-side via PUT /presence/{me}/status
    // which delivers an m.presence to everyone in our shared rooms
    // on their next sync. Empty clears.
    Q_INVOKABLE QString selfStatusMessage() const { return m_selfStatusMessage; }
    Q_INVOKABLE void setSelfStatusMessage(const QString& msg);
    // Per-user lookup. Returns empty if we've never seen a status
    // for this user.
    Q_INVOKABLE QString statusMessageFor(const QString& userId) const;

    // Direct messages — 1:1 rooms with another user on this server.
    // createDirectMessage creates+invites then marks the room as DM
    // locally (persisted per-server in the settings file) so the
    // channel list can segregate them under "Direct Messages". Matrix
    // also ships an `m.direct` account_data event; we accept either
    // signal but for MVP we only write the local store.
    Q_INVOKABLE void createDirectMessage(const QString& targetUserId);
    Q_INVOKABLE bool isDirectRoom(const QString& roomId) const;
    Q_INVOKABLE QString directRoomPeer(const QString& roomId) const;
    // List of all DM rooms in this server, newest-activity first.
    // Each entry: { roomId, peerId, peerDisplayName, lastMessageTime }.
    Q_INVOKABLE QVariantList directRooms() const;

    // Fuzzy search over this connection's known users — everyone
    // whose display name or MXID has crossed our /sync recently
    // (tracked in m_userDisplayNames). Used by the DM composer's
    // live-match list so users don't have to know a peer's full
    // MXID to start a DM.
    // Each entry: { userId, displayName }. `limit` caps results.
    Q_INVOKABLE QVariantList searchKnownUsers(const QString& query,
                                              int limit = 8) const;

    // Server-wide screen-share max quality preset (0..3). Written by
    // admins via a bsfchat.server.screenshare state event; read by
    // every client on sync. Clients clamp their own quality pref
    // downward to this value. Default 3 (Ultra ⇒ no limit) when the
    // server has never set a policy.
    Q_INVOKABLE int maxScreenShareQuality() const { return m_maxScreenShareQuality; }
    Q_INVOKABLE void setMaxScreenShareQuality(int level);
    Q_INVOKABLE void uploadAvatar(const QString& fileUrl);
    Q_INVOKABLE void fetchProfile(const QString& userId);
    Q_INVOKABLE QString resolveMediaUrl(const QString& mxcUri) const;

signals:
    void displayNameChanged();
    void serverNameChanged();
    void serverAvatarUrlChanged();
    // Bumped whenever presence-relevant state changes (activity
    // observed, self-status set). Views use it as a cheap reactive
    // hook since presenceFor() is a lookup on a regular QMap.
    void presenceChanged();
    void directRoomsChanged();
    // Emitted whenever a remote peer's screen-share frame has been
    // updated. QML rebinds peerScreenDataUrl(userId) off this.
    void peerScreenFrameChanged(const QString& userId);
    void peerCameraFrameChanged(const QString& userId);
    void peerLevelChanged(const QString& userId);
    // Forwarded from MatrixClient; QML composer binds to this to show
    // a progress bar while a file attachment is uploading.
    void mediaUploadProgress(const QString& filename, double progress);
    void maxScreenShareQualityChanged();
    void userIdChanged();
    void activeRoomIdChanged();
    void pttPressedChanged();
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
    void viewingVoiceRoomChanged();
    void voiceMutedChanged();
    void voiceDeafenedChanged();
    void voiceMembersChanged();
    void micLevelChanged();
    void micSilentChanged();
    void avatarUrlChanged();
    void typingDisplayChanged();
    // User-facing send feedback — emitted when the server rejects
    // a message with a useful-to-surface error (rate limits, size
    // caps, permission errors). QML subscribes and routes to
    // ToastHost. `kind` is "error" / "warning" / "info".
    void sendFeedback(const QString& text, const QString& kind);
    void roomTypingChanged();
    void roomPinnedEventsChanged(const QString& roomId);
    void profileFetched(const QString& userId, const QString& displayName, const QString& avatarUrl);
    void myPowerLevelChanged();
    void serverRolesChanged();
    void permissionsChanged();
    void categorizedRoomsChanged();
    void bannedMembersChanged();
    void serverMembersChanged();
    // Fired when a state-event write (role assignment, channel override,
    // channel settings, server name) was rejected by the server. The QML
    // layer shows a toast and the optimistic local update is rolled back
    // before this signal fires. `kind` is a short tag ("role-assign",
    // "server-name", …) so the UI can tailor the message.
    void stateWriteFailed(const QString& kind, int status, const QString& error);
    // Emitted when something outside MessageView (e.g. a message-link click
    // from another server, or a cross-room jump) has asked the chat pane to
    // scroll to a specific event. MessageView listens and calls
    // positionViewAtIndex on the matching row, if loaded.
    void scrollToEventRequested(const QString& eventId);
    // Fired after a /messages response is absorbed into the MessageModel.
    // MessageView listens to drive the "paginate-until-found" loop for
    // reply-jumps whose target wasn't in the initial timeline.
    void olderMessagesLoaded();
    // Emitted once per inbound m.room.message timeline event that is
    // attributable to a different user and isn't an edit or reaction.
    // NotificationManager consumes this to raise OS notifications — the
    // signal fires regardless of whether the message is in the active room
    // so cross-server/cross-room notifications are possible.
    void messageReceived(const QString& roomId,
                         const QString& senderDisplayName,
                         const QString& body,
                         const QString& eventId,
                         bool mentionsMe);

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
    QString m_serverAvatarMxc;  // raw mxc:// uri
    QString m_serverAvatarUrl;  // resolved http URL for QML
    QString m_avatarUrl;
    QString m_activeRoomId;
    QString m_activeRoomName;
    QString m_activeRoomTopic;
    bool m_connected = false;
    int m_connectionStatus = 0; // 0=disconnected, 1=connected, 2=reconnecting
    QString m_syncErrorMessage;
    bool m_hasUnread = false;
    bool m_viewingVoiceRoom = false;

public:
    bool viewingVoiceRoom() const { return m_viewingVoiceRoom; }

    // Per-room member cache: roomId -> list of member events
    QMap<QString, QVector<bsfchat::RoomEvent>> m_roomMembers;
    QMap<QString, QStringList> m_roomPinnedEvents;
    QMap<QString, qint64> m_userLastActivityMs; // for activity-based presence
    QString m_selfPresence = QStringLiteral("online");
    QString m_selfStatusMessage;
    // Authoritative presence + status messages from sync, keyed by
    // user_id. Beats the client-side "saw a message recently"
    // heuristic when the server actually relays presence (which it
    // does as of the PresenceHandler addition).
    QMap<QString, QString> m_userPresenceFromSync;
    QMap<QString, QString> m_userStatusMessage;
    // DM store: roomId -> peer user id. Persisted via Settings under
    // "dm/<serverUrl>/<roomId>". Loaded once on setup.
    QMap<QString, QString> m_directRoomPeers;
    QMap<QString, QString> m_peerScreenData; // userId -> data URL
    QMap<QString, QString> m_peerCameraData; // userId -> data URL
    QMap<QString, float> m_peerLevels;
    int m_maxScreenShareQuality = 3; // 3 = no limit by default
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
    // Per-room typing state. Populated for every joined room's
    // ephemeral typing event, not just the active one, so the sidebar
    // can surface a quiet indicator on non-active channels where
    // someone is typing. Set entries are user IDs minus self.
    QMap<QString, QStringList> m_roomTyping;
    int m_typingGeneration = 0;

    // Voice state
    QString m_activeVoiceRoomId;
    float m_micLevel = 0.0f;
    bool m_micSilent = false;
    int m_zeroLevelFrames = 0; // consecutive frames with near-zero level
    bool m_voiceMuted = false;
    bool m_pttPressed = false;
    Settings* m_settings = nullptr;
    void applyMicGate();  // recompute mute from voiceMuted + ptt mode
public:
    // Called once at startup by ServerManager so the mic gate can
    // consult voiceMode / PTT prefs without a global singleton.
    void setSettings(Settings* settings);
private:
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

    // Optimistic-update undo stash, keyed by "<eventType>|<stateKey>". When
    // a state-event write fails, we pop the matching closure and run it to
    // restore the prior cache value; then re-emit permissionsChanged so the
    // QML re-binds to the rolled-back state.
    QMap<QString, std::function<void()>> m_pendingStateUndo;

    // Helper: parse a JSON state event into typed caches. Each applier
    // takes the event's `originMs` (== origin_server_ts) so it can reject
    // stale replays — otherwise a sync that returns member.roles events
    // from multiple rooms can land in arbitrary order and the "newer" event
    // loses to the "older" one depending on iteration.
    void applyServerRolesEvent(const QJsonObject& content, qint64 originMs);
    void applyMemberRolesEvent(const QString& userId, const QJsonObject& content,
                               qint64 originMs);
    void applyChannelPermissionsEvent(const QString& roomId, const QString& stateKey,
                                       const QJsonObject& content, qint64 originMs);
    void applyChannelSettingsEvent(const QString& roomId, const QJsonObject& content,
                                   qint64 originMs);

    // Last-applied timestamps so the state appliers can enforce latest-wins
    // semantics when the same logical state key shows up with multiple
    // events in one sync response.
    qint64 m_serverRolesTs = 0;
    QMap<QString, qint64> m_memberRolesTs;                     // userId -> ms
    QMap<QPair<QString, QString>, qint64> m_channelOverrideTs; // (roomId, key)
    QMap<QString, qint64> m_channelSettingsTs;                 // roomId -> ms

    // Track first sync in this session — only prune hidden rooms once, since
    // subsequent (incremental) syncs only include rooms with new activity.
    bool m_firstSyncProcessed = false;
    QSet<QString> m_knownRoomIds;
};
