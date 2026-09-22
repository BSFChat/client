#pragma once

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QString>
#include <QTimer>

#include "util/HistoryFill.h"
#include "util/MentionRenderer.h"
#include "net/SessionAuth.h"
#include <QVector>
#include <functional>

#include <bsfchat/Constants.h>
#include <bsfchat/MatrixTypes.h>
// Header-only dependency (QString + QJsonObject), safe in non-voice
// builds. The .cpp side of it is voice-gated, so every CALL into
// voice:: below sits behind BSFCHAT_VOICE_ENABLED.
#include "voice/IpPrivacy.h"
#include "voice/VoiceEncryption.h"
#include "voice/VoiceStartPolicy.h"
// The join/leave state machine. Header-only dependency on QObject/QJson
// (CallSignal is a plain value type), so it is safe in non-voice builds
// too — the session runs there as well; it simply has no transport to
// start. See src/net/VoiceSession.h.
#include "net/DeactivateFlow.h"
#include "net/DirectRooms.h"
#include "net/MatrixFailure.h"
#include "net/VoiceSession.h"

class MatrixClient;
class MediaDownloader;
class SyncLoop;
class LocalCache;
class RoomListModel;
class MessageModel;
class MemberListModel;
class BlockedUsersModel;
class BotAdminModel;
class ChannelInviteModel;
class SelfRoleModel;
#ifdef BSFCHAT_VOICE_ENABLED
class VoiceEngine;
class NotificationSounds;
class VideoStreamRegistry;
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
    // Bumped every time a media ticket lands. Media URLs are no longer a pure
    // function of (homeserver, token, mxc) — they carry a short-lived signature
    // that has to be fetched — so resolveMediaUrl() can answer "" now and
    // something real a moment later. A QML binding that calls resolveMediaUrl()
    // must read this too, or it will never re-evaluate and the avatar will stay
    // blank forever. Read AND COMPARED, not merely touched: a bare property read
    // is dead-code-eliminated on QML's AOT-compiled path (same trap as
    // UserProfileCard's _permGen).
    Q_PROPERTY(int mediaTicketEpoch READ mediaTicketEpoch NOTIFY mediaTicketEpochChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY userIdChanged)
    // The composer's character... BYTE budget. See messageByteLength() below
    // for why the composer has to count in bytes, and Constants.h for why the
    // server does.
    //
    // CONSTANT, and the protocol default rather than anything the server told
    // us: there is no endpoint that publishes an operator's configured
    // `[limits] max_message_bytes`, so this is the figure the stock server
    // enforces. On a deployment that has lowered it, the composer lets a
    // message through that the server then refuses with 413 M_TOO_LARGE —
    // which is the pre-existing behaviour for every message, so the cap is
    // strictly an improvement rather than a new failure mode. It must never be
    // set HIGHER than the protocol default for the same reason.
    Q_PROPERTY(int maxMessageBytes READ maxMessageBytes CONSTANT)
    Q_PROPERTY(RoomListModel* roomListModel READ roomListModel CONSTANT)
    Q_PROPERTY(MessageModel* messageModel READ messageModel CONSTANT)
    Q_PROPERTY(MemberListModel* memberListModel READ memberListModel CONSTANT)
    // View-model behind the bot management dialog. Always present, even for a
    // user with no MANAGE_BOTS — the dialog binds to it unconditionally and is
    // itself gated on canManageBots(), so a null here would only move the
    // permission check into every binding inside it.
    Q_PROPERTY(BotAdminModel* botAdminModel READ botAdminModel CONSTANT)
    // View-model behind the member-facing self-assignable role picker. Always
    // present and deliberately ungated: unlike botAdminModel, whose dialog is
    // behind canManageBots(), the whole point of this one is that an ORDINARY
    // member can open it. It reports an empty list on a server that publishes
    // no opt-in roles, which is the only "nothing to see" this surface has.
    Q_PROPERTY(SelfRoleModel* selfRoleModel READ selfRoleModel CONSTANT)
    // View-model behind "add a member to this channel". Always present and
    // bound unconditionally, for the reason botAdminModel gives — and here the
    // dialog is deliberately reachable WITHOUT the permission as well (see
    // AddMemberDialog.qml), so a permission-gated model would defeat the
    // point: the locked state is the thing a member without MANAGE_CHANNELS
    // most needs to be able to reach.
    Q_PROPERTY(ChannelInviteModel* channelInviteModel READ channelInviteModel CONSTANT)
    // View-model behind blocking: who is on this account's m.ignored_user_list,
    // and the toggle behind every Block / Unblock control. Always present and
    // ungated for the reason selfRoleModel is — blocking is a thing every
    // account may do, and it is the control a user reaches for when a
    // moderator is not around.
    //
    // It is NOT fed by /sync: this server sends no account_data there, so the
    // list changes only when something asks. See BlockedUsersModel's header
    // for where those asks happen and what the UI is allowed to claim.
    Q_PROPERTY(BlockedUsersModel* blockedUsersModel READ blockedUsersModel CONSTANT)
    // Bumped whenever the set of known bot user ids changes, so QML that
    // asks isBot(userId) directly (the profile card, which has a user id and
    // no model row) re-evaluates. The member list and the message list do not
    // need it — they carry the flag on their rows and are repainted by the
    // models' own dataChanged.
    Q_PROPERTY(int botFlagsGeneration READ botFlagsGeneration NOTIFY botFlagsChanged)
    Q_PROPERTY(QString activeRoomId READ activeRoomId NOTIFY activeRoomIdChanged)
    Q_PROPERTY(QString activeRoomName READ activeRoomName NOTIFY activeRoomNameChanged)
    Q_PROPERTY(QString activeRoomTopic READ activeRoomTopic NOTIFY activeRoomTopicChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(int connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncErrorMessage READ syncErrorMessage NOTIFY syncErrorMessageChanged)
    // The homeserver has stopped accepting this connection's access token
    // and only a fresh sign-in can revive it. Drives the "Sign in again"
    // button; `reauthInProgress` is true only while the login round trip is
    // actually running, so the button can disable itself instead of
    // launching a second browser flow.
    //
    // Deliberately NOT named sessionExpired — that is a signal on this
    // class, and a property sharing the name would be ambiguous in QML.
    Q_PROPERTY(bool needsReauth READ needsReauth NOTIFY sessionAuthChanged)
    Q_PROPERTY(bool reauthInProgress READ reauthInProgress NOTIFY sessionAuthChanged)
    Q_PROPERTY(bool hasUnread READ hasUnread NOTIFY hasUnreadChanged)
    Q_PROPERTY(QString activeVoiceRoomId READ activeVoiceRoomId NOTIFY activeVoiceRoomIdChanged)
    Q_PROPERTY(bool voiceMuted READ voiceMuted NOTIFY voiceMutedChanged)
    Q_PROPERTY(bool voiceDeafened READ voiceDeafened NOTIFY voiceDeafenedChanged)
    Q_PROPERTY(bool inVoiceChannel READ inVoiceChannel NOTIFY activeVoiceRoomIdChanged)
    // Last voice-subsystem error (join/leave REST failures, engine
    // init failures). Auto-clears after ~8 s or on the next successful
    // join; QML routes non-empty values into the toast surface.
    Q_PROPERTY(QString voiceError READ voiceError NOTIFY voiceErrorChanged)
    // Which view the main area should display. Independent of whether
    // the user is connected to a voice call — you can be in voice AND
    // reading a text channel. Flips true when the voice channel row is
    // clicked (or the VoiceDock / VoiceStatusCard is tapped), false
    // whenever a text channel becomes active or voice is left.
    Q_PROPERTY(bool viewingVoiceRoom READ viewingVoiceRoom NOTIFY viewingVoiceRoomChanged)
    Q_PROPERTY(QJsonArray voiceMembers READ voiceMembers NOTIFY voiceMembersChanged)
    // How the CURRENT call's media is protected, as the badge string for
    // the VoiceRoom header and the long form for its tooltip. Empty
    // strings when no session is live, so QML hides the badge rather
    // than asserting a property nothing is providing.
    //
    // Both strings come from voice::protectionBadge/protectionDetail and
    // are NOT written here. That indirection is the point: the QML badge
    // used to hard-code "DTLS · SCTP", which would have kept claiming the
    // mesh protocol on an SFU call. Read src/voice/VoiceEncryption.h
    // before touching any of this — the wording rules live there.
    Q_PROPERTY(QString voiceProtectionBadge READ voiceProtectionBadge
                   NOTIFY voiceProtectionChanged)
    Q_PROPERTY(QString voiceProtectionDetail READ voiceProtectionDetail
                   NOTIFY voiceProtectionChanged)
    // "Hide my IP address": the LIVE state, not the setting. Same rule and
    // the same reason as the two above — the strings are written in
    // src/voice/IpPrivacy.cpp and reach QML only through these properties,
    // because a claim composed in QML is a claim nothing can check.
    //
    // Empty badge means "say nothing": no session, or a direct call, which is
    // the default and is not a warning. The detail is never empty while a
    // session is live, because the honest description of a direct call is
    // still worth reading.
    Q_PROPERTY(QString voiceIpPrivacyBadge READ voiceIpPrivacyBadge
                   NOTIFY voiceIpPrivacyChanged)
    Q_PROPERTY(QString voiceIpPrivacyDetail READ voiceIpPrivacyDetail
                   NOTIFY voiceIpPrivacyChanged)
    // Remote-video surface registry (VideoStreamRegistry; null when
    // voice is compiled out). QML attaches VideoOutput sinks via its
    // invokables — see VoiceRoom / ParticipantTile.
    Q_PROPERTY(QObject* videoRegistry READ videoRegistryObject CONSTANT)
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
    // This account's name as every "who am I signed in as" surface prints
    // it. Never the bare mxid — see the definition for why that is the
    // accessor's job and not each caller's.
    QString displayName() const;
    QString avatarUrl() const { return m_avatarUrl; }
    QString serverUrl() const { return m_serverUrl; }
    QString serverName() const;
    QString serverAvatarUrl() const { return m_serverAvatarUrl; }
    QString userId() const { return m_userId; }

    int maxMessageBytes() const {
        return static_cast<int>(bsfchat::limits::kMaxMessageBodyBytes);
    }

    // UTF-8 bytes in `text`, which is what the server counts and what QML
    // cannot work out for itself.
    //
    // QML has no byte length. `text.length` is UTF-16 code UNITS: one for most
    // characters, two for anything outside the BMP, and in neither case the
    // number the server is comparing against. Counting units would have the
    // composer tell a Japanese speaker they have 16,000 characters left when
    // the server will take about 5,400, and tell an emoji-heavy user 16,000
    // when the real answer is ~4,000 — the composer's whole purpose is to say
    // no BEFORE the send, so a cap that disagrees with the server is worse
    // than none.
    //
    // Cheap enough to call on every keystroke: QString::toUtf8 on a composer's
    // worth of text is a single pass over a few kilobytes.
    Q_INVOKABLE int messageByteLength(const QString& text) const {
        return static_cast<int>(text.toUtf8().size());
    }

    QString accessToken() const { return m_accessToken; }
    QString deviceId() const { return m_deviceId; }
    bool isConnected() const { return m_connected; }
    QString activeRoomId() const { return m_activeRoomId; }
    QString activeRoomName() const { return m_activeRoomName; }
    QString activeRoomTopic() const { return m_activeRoomTopic; }

    // 0 = disconnected, 1 = connected/syncing, 2 = reconnecting,
    // 3 = session expired (see needsReauth)
    int connectionStatus() const { return m_connectionStatus; }
    QString syncErrorMessage() const { return m_syncErrorMessage; }
    bool needsReauth() const { return m_auth.needsReauth(); }
    bool reauthInProgress() const { return m_auth.isReauthenticating(); }
    // What a "reconnect" request should do about this connection. Owned by
    // SessionAuth so the rule — a token the server has already rejected can
    // never be retried into working — is pinned by tests/test_session_auth.cpp
    // rather than re-derived at the call site. ServerManager dispatches on it.
    bsfchat::client::ReconnectAction reconnectAction() const
    {
        return m_auth.actionForReconnect();
    }
    bool hasUnread() const { return m_hasUnread; }

    QString activeVoiceRoomId() const { return m_activeVoiceRoomId; }
    QString voiceError() const { return m_voiceError; }
    bool voiceMuted() const { return m_voiceMuted; }
    bool voiceDeafened() const { return m_voiceDeafened; }
    bool inVoiceChannel() const { return !m_activeVoiceRoomId.isEmpty(); }

    // Empty until a transport has actually started, and empty again the
    // moment it stops. Deliberately not "assume mesh until told
    // otherwise": a badge shown before the session exists is a claim
    // about a connection that has not been made.
    QString voiceProtectionBadge() const;
    QString voiceProtectionDetail() const;
    QString voiceIpPrivacyBadge() const;
    QString voiceIpPrivacyDetail() const;
    // Pushes Settings::voiceRelayMode into the live engine (and remembers it
    // for the next start). Connected to the setting's change signal, so
    // turning it on mid-call takes effect on this call.
    void applyVoiceRelayMode();
    // The per-share "Hide my IP while sharing" override, routed from the
    // screen-share and camera controllers. `stream` picks which one; the
    // engine relays while either is set, because one peer connection carries
    // voice and both video streams.
    Q_INVOKABLE void setShareIpPrivacy(int stream, bool hideIp);
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
    // Remote video render path: every incoming stream (decoded RTP
    // video AND legacy JPEG stills) lands on a per-peer QVideoSink in
    // the VideoStreamRegistry; QML tiles attach VideoOutputs to it.
    // Returned as QObject* so non-voice builds don't need the type —
    // null when voice is compiled out.
    QObject* videoRegistryObject() const;
#ifdef BSFCHAT_VOICE_ENABLED
    VideoStreamRegistry* videoRegistry() const { return m_videoRegistry; }
#endif
    // Union of peers whose polled voice-member row announces
    // screen_sharing=true and anyone with live frames in the registry.
    // The announcement is the fast/authoritative path (tiles appear
    // before the first frame lands); the live-frame fallback keeps a
    // share visible even if a flag update got lost.
    Q_INVOKABLE QStringList peersCurrentlySharing() const;
    // Announced camera flag OR live camera frames — same union
    // semantics as peersCurrentlySharing().
    Q_INVOKABLE bool peerHasCamera(const QString& userId) const;

    // 0..1 smoothed audio level for a remote peer, updated on every
    // decoded Opus frame. ParticipantTile binds its speaking ring to
    // this. Empty for unknown/silent peers.
    Q_INVOKABLE float peerLevel(const QString& userId) const {
        return m_peerLevels.value(userId, 0.0f);
    }
    // Cumulative receive stats for a peer's video stream (0 = screen,
    // 1 = camera); the diagnostics overlay polls this once a second
    // and diffs snapshots into fps / bitrate. Empty map when the
    // stream has no receive pipeline (nothing received yet).
    Q_INVOKABLE QVariantMap videoReceiveStats(const QString& userId,
                                              int streamId) const;
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
    int botFlagsGeneration() const { return m_botFlagsGeneration; }

    RoomListModel* roomListModel() const { return m_roomListModel; }
    MessageModel* messageModel() const { return m_messageModel; }
    MemberListModel* memberListModel() const { return m_memberListModel; }
    BotAdminModel* botAdminModel() const { return m_botAdminModel; }
    SelfRoleModel* selfRoleModel() const { return m_selfRoleModel; }
    ChannelInviteModel* channelInviteModel() const { return m_channelInviteModel; }
    BlockedUsersModel* blockedUsersModel() const { return m_blockedUsersModel; }
    MatrixClient* client() const { return m_client; }

    // Set credentials (for restoring from settings)
    void setCredentials(const QString& userId, const QString& accessToken,
                        const QString& deviceId, const QString& displayName);

    // Actions
    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void registerUser(const QString& username, const QString& password);
    Q_INVOKABLE void disconnectFromServer();
    Q_INVOKABLE void setActiveRoom(const QString& roomId);

    // ── THE READ MARKER IS THE VIEW'S CALL, NOT THE ROOM SWITCH'S ────
    //
    // U-M3. Opening a room used to reset its unread count and POST a read
    // marker straight away, and leaving it wrote the newest LOADED
    // message's timestamp into settings — both regardless of whether the
    // user had seen any of it. Open a busy channel, read the top two
    // messages, switch away: everything below was marked read, the badge
    // was gone, and the "New messages" divider never came back.
    //
    // MessageView calls markRoomRead() when, and only when, its viewport
    // is parked at the end of the timeline, so "read" means what it says.
    // setTimelineAtBottom() mirrors that same sample into this object so
    // the room-switch path — which has to persist before the model is
    // cleared, and therefore cannot ask the view — can tell whether the
    // user was caught up when they left.
    Q_INVOKABLE void markRoomRead(const QString& roomId, qint64 tsMs);
    Q_INVOKABLE void setTimelineAtBottom(bool atBottom);

    // Open the channel the user last had open on THIS server, falling back to
    // its first text channel. No-op if a channel is already open, so it is
    // safe to call from every "this server came to the foreground" path.
    //
    // If the remembered channel is not in the room list yet and the initial
    // /sync hasn't landed, this arms a retry instead of falling back — landing
    // on a fallback would also overwrite the memory, which is how the
    // remembered channel gets forgotten permanently.
    Q_INVOKABLE void restoreLastTextRoom();
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
    //
    // `formattedBody`/`mentionedUserIds` are optional and are carried into
    // m.new_content so an edit doesn't strip the message's mention highlight.
    // They cannot ADD a mention badge — the server refuses to record mention
    // rows for an m.replace, by design. See MatrixClient::editMessage.
    Q_INVOKABLE void editMessage(const QString& eventId, const QString& newBody,
                                  const QString& formattedBody = QString(),
                                  const QStringList& mentionedUserIds = {});
    // Send `body` into the currently-active room as a reply pointing at
    // `targetEventId`. The UI composer calls this when the reply banner is
    // active; no server-side support is needed beyond arbitrary content.
    //
    // Mentions in a reply are honoured in full (badge + push), because a reply
    // is an ordinary new message as far as the server's mention pipeline goes.
    Q_INVOKABLE void replyToMessage(const QString& targetEventId, const QString& body,
                                     const QString& formattedBody = QString(),
                                     const QStringList& mentionedUserIds = {});
    // Forward the message identified by sourceEventId (in the active room)
    // into destRoomId on this server. Builds the attribution prefix from
    // the message model + RoomListModel.
    Q_INVOKABLE void forwardMessage(const QString& sourceEventId, const QString& destRoomId);

    // Back-pagination. Issues GET /rooms/{id}/messages?from=<prev_batch>&dir=b
    // — as many pages as it takes to add kHistoryMoreTargetRows VISIBLE rows,
    // capped (util/HistoryFill.h), because a page can be nothing but edits.
    // `limit` sizes the first page. No-op if we've already reached the start
    // of the room or a fill is already running. MessageView binds
    // hasMoreHistory/loadingHistory on MessageModel to drive the
    // scroll-to-top trigger. This is the USER's request (scroll to top, reply
    // jump, the load button) and is not charged to the automatic budget.
    Q_INVOKABLE void loadOlderMessages(int limit = 50);
    // The same, asked by MessageView on its own when the loaded rows do not
    // fill the viewport: a list that cannot scroll can never reach the
    // scroll-to-top trigger. Charged to the per-visit automatic budget, and
    // a no-op once it is spent.
    Q_INVOKABLE void fillHistoryForViewport();

    // Emoji reactions (Matrix m.reaction / m.annotation). If the current
    // user already has a reaction with `emoji` on `targetEventId`, redact it
    // (un-react); otherwise send a new reaction.
    Q_INVOKABLE void toggleReaction(const QString& targetEventId, const QString& emoji);

    // Look up a text channel by its display name (case-insensitive) and
    // make it active. No-op if there's no match. Used by the
    // #channel-mention click handler.
    Q_INVOKABLE void activateRoomByName(const QString& name);

    // ── Server-backed message search ──────────────────────────────────────
    //
    // Issues POST /_matrix/client/v3/search. Results arrive on
    // searchResultsReady / searchErrored, never as a return value — a search box
    // fires one of these per keystroke.
    //
    // `nextBatch` empty requests the first page. Pass the `nextBatch` from a
    // previous searchResultsReady to append the following page; the signal
    // reports `appended` so the UI knows whether to replace or extend its list.
    //
    // The server does the permission filtering (only channels you can see) and
    // the query sanitising, so there is nothing to escape here.
    Q_INVOKABLE void searchMessages(const QString& searchTerm, int limit = 30,
                                     const QString& nextBatch = QString());
    // Switch to `roomId` (if not already active) and then ask the message view
    // to scroll to `eventId`. Search spans every channel you can see, so a
    // result is frequently not in the room currently on screen.
    Q_INVOKABLE void jumpToRoomEvent(const QString& roomId, const QString& eventId);

    // ── Per-room notification level ───────────────────────────────────────
    //
    // "all" / "mentions" / "none". This was local-only (QSettings
    // notifMode/<roomId>), which meant the server had no idea what to push and
    // the choice didn't follow the user to another device.
    //
    // The local copy is kept as the synchronous read path — NotificationManager
    // consults it on every inbound message and cannot wait on a round trip — so
    // it becomes a cache of the server's value rather than the source of truth.
    // `setRoomNotifyLevel` writes locally first (so the UI is instant) and then
    // PUTs; a rejected PUT rolls the local value back and reports via
    // notifyLevelFailed, which main.qml turns into a toast — the rollback
    // alone is silent, because the menu it corrects has already closed.
    Q_INVOKABLE void setRoomNotifyLevel(const QString& roomId, const QString& level);
    // Pull the server's value for one room. Fires notifyLevelChanged once the
    // answer lands (and only if it actually differs from the local cache).
    Q_INVOKABLE void refreshRoomNotifyLevel(const QString& roomId);

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
    // Discard whatever credential we are holding and run the server's
    // current login flow from scratch. The only thing that recovers a
    // session the homeserver has purged.
    //
    // The identity provider is re-read from GET /login on every attempt and
    // never taken from the persisted server entry. That field is written
    // once, at first login, from the identity-first flow's URL — so an entry
    // created by a password login holds an empty string, and any server that
    // later changes IdP strands every client that stored the old one.
    Q_INVOKABLE void beginReauth();
    // Second half of beginReauth() for a server that advertises only
    // m.login.password: the UI collects the credentials and hands them back.
    Q_INVOKABLE void reauthWithPassword(const QString& username,
                                        const QString& password);
    // Returns the identity-provider base URL (e.g. "https://id.bsfchat.com")
    // for the "Manage Account" link: the live OIDC session's provider if
    // this process ran the flow, otherwise the one this connection was
    // restored with. Empty only when neither is known.
    Q_INVOKABLE QString identityProviderUrl() const;
    // Seeds the value above from a saved server entry. ServerManager calls
    // this wherever it builds a connection it isn't going to log in
    // interactively — the restore loop and rebuildConnection. Without it
    // "Manage Account" sent every restored session to the hosted portal,
    // which is the wrong one for a self-hosted identity provider.
    //
    // Deliberately NOT a source for re-authentication, which must re-read
    // the login flows from the server on every attempt: a provider stored
    // once at first login goes stale the moment a deployment changes its
    // IdP, and reading it back there would make a live trap of it. The
    // "Manage Account" link is a different question — an out-of-date
    // portal address is a wrong link, not a session nobody can recover.
    void setIdentityProviderUrl(const QString& url);

    Q_INVOKABLE void joinVoiceChannel(const QString& roomId);
    Q_INVOKABLE void leaveVoiceChannel();
    // True while this connection still holds, or is winding down, a voice
    // session. The quit path uses it to decide whether to wait (V-H3).
    bool isLeavingVoice() const;
    // The session object, so ServerManager can wait on its `settled`
    // signal at quit. Never null.
    VoiceSession* voiceSession() const { return m_voiceSession; }
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
    // Announce (or retract) local screen-share / camera activity to
    // the server via a media-flag-only voice/state PUT, so remote
    // clients can render "peer is sharing" before any frame arrives.
    // No-op when not in voice (the server would 403 it anyway).
    // Wired from main.cpp off the controllers' activeChanged signals.
    Q_INVOKABLE void setLocalMediaState(bool screenSharing, bool cameraOn);
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
    // The same computation for SOMEBODY ELSE. Needed by the role-mention
    // resolver, which has to answer "did the sender hold MENTION_EVERYONE?" to
    // know whether a non-mentionable role in their message actually pinged
    // anyone. Degrades safely: a user whose bsfchat.member.roles this client has
    // not seen resolves as @everyone only, so the answer errs towards "not
    // permitted" and the mention renders as plain text rather than as a pill
    // for a ping that may not have happened.
    //
    // For a BOT sender it errs further in that same safe direction: a bot does
    // not inherit the implicit @everyone role at all
    // (permmath::inheritsEveryoneRole, mirroring the server's
    // permission::inherits_everyone_role), so an unseen assignment resolves to
    // nothing rather than to the server's defaults. That is what stops this
    // client rendering a bot's literal "@everyone" as a live room ping the
    // server had already refused to deliver as one.
    quint64 permissionsFor(const QString& userId, const QString& roomId) const;

    // Resolve the role ids an event named into the role mentions that actually
    // notified somebody. Mirrors the server's rule; see the resolver's
    // definition in the .cpp for the rule and why the client re-derives it.
    QVector<bsfchat::client::RoleMentionTarget> resolveRoleMentions(
        const QStringList& roleIds, const QString& sender, const QString& roomId) const;

    Q_INVOKABLE bool canSend(const QString& roomId) const;
    Q_INVOKABLE bool canAttach(const QString& roomId) const;
    Q_INVOKABLE bool canEmbed(const QString& roomId) const;
    Q_INVOKABLE bool canManageChannel(const QString& roomId) const;
    Q_INVOKABLE bool canManageRoles(const QString& roomId) const;
    // Server-wide, as on the server: the room id is accepted for call-site
    // uniformity but ignored. See the definitions.
    Q_INVOKABLE bool canKick(const QString& roomId = QString()) const;
    Q_INVOKABLE bool canBan(const QString& roomId = QString()) const;
    Q_INVOKABLE bool canManageMessages(const QString& roomId) const;
    // Creating a channel or category is server structure, not a per-channel
    // action, so it takes no room id — see the definition for why that matters.
    Q_INVOKABLE bool canCreateChannels() const;
    // Nicknames are one value for the whole server, so both of these are
    // SERVER-SCOPE questions and take no room id: the server evaluates them with
    // no channel, and passing one here would let a per-channel override light up
    // an affordance the server then refuses.
    //
    // These are deliberately two independent questions rather than a hierarchy,
    // matching the server: MANAGE_NICKNAMES permits renaming OTHER people and
    // does not imply the right to rename yourself.
    Q_INVOKABLE bool canChangeNickname() const;
    Q_INVOKABLE bool canManageNicknames() const;
    // MANAGE_BOTS, at SERVER scope for the same reason as the two above: a bot
    // belongs to the server, not to a channel, and the server evaluates the
    // flag with no room. Passing a room id here would let a per-channel
    // override open a dialog whose every request the server then refuses.
    Q_INVOKABLE bool canManageBots() const;
    // MANAGE_SERVER — the server name/icon and the audit log, which is the
    // Overview page of Server Settings. Server scope, like the three above.
    Q_INVOKABLE bool canManageServer() const;
    // "Does Server Settings open onto anything you may use?" — the roll-up
    // behind the header gear.
    //
    // One question rather than the four the gear used to ask inline, because
    // the gear's gate and the modal's page list are the same fact and they had
    // already drifted apart: MANAGE_SERVER and MANAGE_BOTS each open a page
    // that no combination of the old four could reach. The set lives in
    // permmath::kServerSettingsPages so it is checkable without a live
    // connection.
    //
    // A `false` here does NOT mean "hide the gear" — see ChannelList.qml. It
    // means the modal opens on its locked state.
    Q_INVOKABLE bool canOpenServerSettings() const;

    // ── Bot identity ──────────────────────────────────────────────────────

    // Whether `userId` is a bot account. Never asks the network and never
    // can: the flag rides in `bsfchat.bot` on every m.room.member event the
    // server serves, so knowing a user exists on this server is the same as
    // knowing whether they are a bot. Safe from a paint-time binding.
    Q_INVOKABLE bool isBot(const QString& userId) const;
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

    // ── Blocking and reporting ────────────────────────────────────────────
    //
    // The pair a user reaches for when another account is the problem. They do
    // different things and the UI must offer both, because neither is the
    // other:
    //
    //   * BLOCK is instant, private and needs nobody's help. The server stops
    //     delivering that account's messages, mentions, unread counts and push
    //     to this one. The blocked party is never told.
    //   * REPORT changes nothing at all. It files a row for a server
    //     administrator to act on later, by hand. Nothing is redacted, nobody
    //     is muted, and the reported account learns nothing.
    //
    // Neither is moderation and neither is permission-gated: every account may
    // do both, including an account with no roles in a channel it cannot
    // moderate. That is the point of them.

    // Whether `userId` is on this account's block list. Never asks the
    // network — safe from a paint-time binding — and counts a write in flight
    // as already landed so a menu does not flicker mid-round-trip.
    //
    // False for everyone until the list has been fetched once. It cannot be
    // otherwise: there is no block list in /sync to have arrived with the
    // session. Callers that care (the managed pane) read
    // blockedUsersModel.loaded.
    Q_INVOKABLE bool isUserBlocked(const QString& userId) const;
    Q_INVOKABLE void blockUser(const QString& userId);
    Q_INVOKABLE void unblockUser(const QString& userId);
    // Re-read the list from the server. There is no push for this document, so
    // this is the ONLY thing that learns about a block made on another device.
    // Called on connect, after our own writes, and from the managed pane.
    Q_INVOKABLE void refreshBlockedUsers();

    // File a report. `reason` may be empty — a report with nothing said is
    // still a report, and demanding an explanation from somebody who has just
    // been abused is its own kind of failure. Answers through reportFiled /
    // reportRejected, both carrying `requestId` so a reply that lands after
    // the dialog was closed and reopened for a different target is not
    // attributed to the new one.
    //
    // reportMessage takes the ACTIVE room's id from the connection rather than
    // from the caller: the event id alone does not identify a room to the
    // server, and a QML delegate that passed one would be passing the room it
    // was built in, which is not necessarily the room on screen when the
    // dialog was submitted.
    Q_INVOKABLE void reportMessage(const QString& requestId, const QString& roomId,
                                   const QString& eventId, const QString& reason);
    Q_INVOKABLE void reportUser(const QString& requestId, const QString& userId,
                                const QString& reason);

    // ── Account deletion ──────────────────────────────────────────────────
    //
    // Apple App Store guideline 5.1.1(v): an app that lets people create an
    // account must let them delete it from inside the app. This is that.
    //
    // THE CONFIRMATION MUST BE COMPLETE BEFORE beginAccountDeletion() IS
    // CALLED. For an account with no password on this server — one signed in
    // through the identity provider — the first request deletes it outright;
    // the password prompt is a second gate that only password accounts ever
    // see, and a UI that posted in order to find out which kind it was dealing
    // with would delete the other kind without asking. net/DeactivateFlow.h
    // carries the same warning at greater length.
    Q_INVOKABLE void beginAccountDeletion();
    // Answer the m.login.password stage. Only meaningful after
    // accountDeletionPasswordRequired.
    Q_INVOKABLE void submitAccountDeletionPassword(const QString& password);
    // The user closed the dialog. Abandons a challenge that has not been
    // answered; it cannot abandon a deletion that has already succeeded.
    Q_INVOKABLE void cancelAccountDeletion();
    // True while a deactivate request is in flight — the dialog disables its
    // buttons on this.
    Q_PROPERTY(bool accountDeletionBusy READ accountDeletionBusy
               NOTIFY accountDeletionChanged)
    // True once the server has asked for a password and until the flow is
    // reset. Stays true WHILE the password is being checked, so the field does
    // not vanish under a wrong-password message.
    Q_PROPERTY(bool accountDeletionNeedsPassword READ accountDeletionNeedsPassword
               NOTIFY accountDeletionChanged)
    // Last refusal, for the inline error line. Empty when nothing has failed.
    Q_PROPERTY(QString accountDeletionError READ accountDeletionError
               NOTIFY accountDeletionChanged)
    bool accountDeletionBusy() const { return m_deactivate.busy(); }
    bool accountDeletionNeedsPassword() const { return m_deactivate.needsPassword(); }
    QString accountDeletionError() const { return m_deactivate.errorText(); }

    // Server-scope moderation. NOT symmetric, on purpose:
    //   * ban / unban are ONE request — the server keeps a real server-wide ban
    //     list and projects the membership across every room itself, including
    //     rooms this client has never synced. The room id is audit context.
    //   * kick is one request PER synced channel the user is in, because a kick
    //     is genuinely per-channel server-side (matches Discord).
    // The rule and its justification live in util/ModerationScope.h; the .cpp
    // side of these three is a loop over what it returns.
    Q_INVOKABLE void kickFromServer(const QString& userId, const QString& reason = {});
    Q_INVOKABLE void banFromServer(const QString& userId, const QString& reason = {});
    Q_INVOKABLE void unbanFromServer(const QString& userId);

    // Getter for the Q_PROPERTY declared above. Returns the snapshot last
    // published by refreshMemberSnapshots(). Entries:
    //   { userId, displayName, rooms: [roomId...], reason }
    QVariantList bannedMembers() const { return m_bannedMembers; }
    // Server-wide members union. Entries:
    //   { userId, displayName, avatarUrl, rooms: [roomId...] }
    // Only users whose LATEST membership per room is "join" are included;
    // a user who's "join" in one room and "leave" in another still counts
    // as long as at least one room shows them as joined.
    QVariantList serverMembers() const { return m_serverMembers; }

    Q_INVOKABLE void updateDisplayName(const QString& name);
    // Update the server-wide name (bsfchat.server.info). Requires MANAGE_SERVER.
    Q_INVOKABLE void updateServerName(const QString& name);
    Q_INVOKABLE void updateAvatarUrl(const QString& url);

    // Per-server nickname. `userId` may be ourselves or another member; the
    // server requires CHANGE_NICKNAME for the former and MANAGE_NICKNAMES plus a
    // rank check for the latter. An EMPTY `nickname` clears it.
    //
    // Unlike updateDisplayName, which edits the global profile, this changes only
    // how the user is named on THIS server. Rejections arrive as sendFeedback, so
    // callers do not need their own error handling.
    Q_INVOKABLE void setNickname(const QString& userId, const QString& nickname);
    // Reads the current nickname so an editor can prefill it and offer "clear".
    // Answers via nicknameFetched. Needed because member events carry the
    // EFFECTIVE name: without this a UI cannot tell a nickname apart from a
    // global display name, and so cannot know whether there is one to remove.
    Q_INVOKABLE void fetchNickname(const QString& userId);

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
    // Two sources say a room is a DM, and either is enough: the server's
    // `m.direct` account data in /sync (the only way the INVITED side ever
    // finds out), and our own successful create. Both are written through to
    // a per-server QSettings group so the section is there before the first
    // sync, and against a server too old to send m.direct.
    //
    // createDirectMessage is "open the DM with this person", not "create a
    // room": it jumps to the existing room when there is one, does nothing
    // when a create for that peer is already in flight (its reply will land
    // the user in the room), and only otherwise POSTs /createRoom. Callers
    // must NOT pre-scan directRooms() themselves — that check lived in QML
    // once, three of five entry points forgot it, and the other two raced.
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

    // Per-axis screen-share caps, settable by a server admin via
    // the bsfchat.server.screenshare state event. -1 means "no
    // cap"; the client treats those as effectively unlimited and
    // honours the user's setting verbatim. Older servers that
    // only set the legacy `max_quality` preset will continue to
    // see -1 here, which is correct: their preset cap still
    // applies via maxScreenShareQuality().
    Q_PROPERTY(int maxScreenShareFps READ maxScreenShareFps NOTIFY maxScreenSharePolicyChanged)
    Q_PROPERTY(int maxScreenShareWidth READ maxScreenShareWidth NOTIFY maxScreenSharePolicyChanged)
    Q_PROPERTY(int maxScreenShareJpeg READ maxScreenShareJpeg NOTIFY maxScreenSharePolicyChanged)
    // Video-era policy axes: bitrate cap (kbps, -1 = uncapped) and
    // whether the AV1 lossless tier may be used on this server.
    Q_PROPERTY(int maxScreenShareBitrate READ maxScreenShareBitrate NOTIFY maxScreenSharePolicyChanged)
    Q_PROPERTY(bool allowLossless READ allowLossless NOTIFY maxScreenSharePolicyChanged)
    int maxScreenShareFps() const { return m_maxScreenShareFps; }
    int maxScreenShareWidth() const { return m_maxScreenShareWidth; }
    int maxScreenShareJpeg() const { return m_maxScreenShareJpeg; }
    int maxScreenShareBitrate() const { return m_maxScreenShareBitrate; }
    bool allowLossless() const { return m_allowLossless; }
    Q_INVOKABLE void setScreenSharePolicy(int maxFps, int maxWidth,
                                          int maxJpeg, int maxBitrateKbps,
                                          bool allowLossless);
    Q_INVOKABLE void setMaxScreenShareQuality(int level);
    Q_INVOKABLE void uploadAvatar(const QString& fileUrl);
    Q_INVOKABLE void fetchProfile(const QString& userId);
    // The download URL for an mxc URI, or "" when no ticket has been minted for
    // it yet. See mediaTicketEpoch for what a caller has to do about that.
    Q_INVOKABLE QString resolveMediaUrl(const QString& mxcUri) const;
    int mediaTicketEpoch() const { return m_mediaTicketEpoch; }

    // Fetch `mxcUri` to the local media cache and hand THE LOCAL FILE to the
    // desktop, rather than handing a URL to the system browser.
    //
    // This is what replaced Qt.openUrlExternally(mediaUrl) at the four media
    // call sites (the file card and the image escape hatch in MessageBubble,
    // "Open in browser" in ImageViewer, and the video card's middle-click). That
    // call put the media URL — and, before tickets, the viewer's 90-day session
    // token with it — into the system browser's address bar, history and
    // clipboard, and it is the click that completed the account-takeover chain
    // in audit finding A1. Nothing about this path involves a browser or a URL
    // the user can see.
    //
    // The fetch carries an Authorization header, because a C++ downloader can:
    // it does not need a ticket and does not use one.
    Q_INVOKABLE void openMediaExternally(const QString& mxcUri);

signals:
    void displayNameChanged();
    void serverNameChanged();
    void serverAvatarUrlChanged();
    void mediaTicketEpochChanged();
    // The external-open path could not produce a local file. QML surfaces it
    // the same way a failed media send is surfaced.
    void mediaOpenFailed(QString error);
    // Bumped whenever presence-relevant state changes (activity
    // observed, self-status set). Views use it as a cheap reactive
    // hook since presenceFor() is a lookup on a regular QMap.
    void presenceChanged();
    void directRoomsChanged();
    // Emitted when a peer's screen stream goes live or dark (NOT per
    // frame — frames flow through the VideoStreamRegistry sinks). QML
    // re-evaluates peersCurrentlySharing() off this.
    void peerScreenFrameChanged(const QString& userId);
    void peerCameraFrameChanged(const QString& userId);
    void peerLevelChanged(const QString& userId);
    // Forwarded from MatrixClient; QML composer binds to this to show
    // a progress bar while a file attachment is uploading.
    void mediaUploadProgress(const QString& filename, double progress);
    void maxScreenShareQualityChanged();
    void maxScreenSharePolicyChanged();
    void userIdChanged();
    void activeRoomIdChanged();
    void pttPressedChanged();
    void activeRoomNameChanged();
    void connectedChanged();
    // The homeserver has stopped accepting our access token (expired,
    // revoked, signed out elsewhere). Sync has been stopped; only a fresh
    // login can revive this connection. ServerManager surfaces it.
    void sessionExpired(const QString& serverUrl);
    // needsReauth / reauthInProgress moved.
    void sessionAuthChanged();
    // beginReauth() found no OIDC flow but the server does take passwords.
    // The UI must collect them and call reauthWithPassword().
    void reauthPasswordRequired(const QString& serverUrl, const QString& userId);
    // A sign-in attempt failed. Distinct from loginFailed on purpose:
    // ServerManager answers loginFailed by REMOVING the connection, which is
    // right for a server being added and catastrophic for one the user has
    // been using for months.
    void reauthFailed(const QString& error);
    void connectionStatusChanged();
    void syncErrorMessageChanged();
    void activeRoomTopicChanged();
    void hasUnreadChanged();
    void loginSucceeded();
    void loginFailed(const QString& error);
    // Fired when whoami reveals the persisted user id differs from the
    // server's canonical one (stale/corrupt settings). ServerManager
    // listens and rewrites the stored entry.
    void identityCorrected();
    void registerSucceeded();
    void registerFailed(const QString& error);
    void mediaSendCompleted();
    // A COMPOSER attachment failed — and nothing else.
    //
    // MessageInput's in-flight count is decremented on this and on
    // mediaSendCompleted only, and that one fact sets three rules for this
    // signal. Two of them were learned the hard way; both failures looked
    // identical from the outside, which is the reason they are written down
    // together:
    //
    //   WHOSE. The pair is the composer's alone. A receiver of a bare signal
    //   cannot tell which upload it describes, so any OTHER upload borrowing
    //   this one lands a decrement on a count it never incremented — the
    //   composer unlocks and drops "Uploading…" with the user's file still
    //   going. The avatar and server-icon uploads used to do exactly that on
    //   their failure path; they have avatarUploadFailed below.
    //
    //   THAT it fires. It must fire even when the text is worthless: a
    //   swallowed failure leaves the count above zero and the composer
    //   disabled for the rest of the session.
    //
    //   WHEN it fires. Not too early, either. A caller counts the upload on
    //   the line AFTER sendMediaMessage returns, so an emit from inside that
    //   call is a decrement nobody is there to receive, and the increment
    //   that follows it never comes off again.
    //
    // Emit request failures through emitMediaSendFailed and pre-flight ones
    // through emitPreflightMediaFailure; never emit this directly. An upload
    // that is not the composer's does not belong on this signal at all.
    void mediaSendFailed(const QString& error);
    // An avatar upload failed — the user's own (uploadAvatar) or the
    // server's icon (uploadServerAvatar). Separate from mediaSendFailed
    // because these two are not composer uploads and must not touch the
    // composer's bookkeeping; main.qml toasts both the same way, which is
    // all they ever actually shared. Emit through emitAvatarUploadFailed.
    //
    // There is deliberately no avatarUploadCompleted: success on these
    // paths continues into setRoomState / updateAvatarUrl and the user sees
    // the new image. That asymmetry is why only the failure direction ever
    // bled into the composer.
    void avatarUploadFailed(const QString& error);
    void activeVoiceRoomIdChanged();
    // The server retired our voice row and the V-H2 re-join was
    // accepted: same room and same engine, but a NEW membership whose
    // screen_sharing/camera_on both start false. activeVoiceRoomId does
    // not change across it, so anything that announces media state on
    // joining needs this second edge as well — otherwise a live share
    // silently disappears from everyone else's roster.
    void voiceMembershipRenewed();
    void voiceErrorChanged();
    void viewingVoiceRoomChanged();
    void voiceMutedChanged();
    void voiceDeafenedChanged();
    void voiceMembersChanged();
    void voiceProtectionChanged();
    void voiceIpPrivacyChanged();
    void micLevelChanged();
    void micSilentChanged();
    void avatarUrlChanged();
    void typingDisplayChanged();
    // ── Blocking / reporting / deletion ───────────────────────────────────

    // A report landed, or was refused. `requestId` echoes the request so a
    // dialog can ignore a reply to an attempt it no longer owns; `message` on
    // the refusal is already user-facing, rate-limit wait included.
    void reportFiled(const QString& requestId);
    void reportRejected(const QString& requestId, const QString& message);

    // Any change to the deletion flow's phase or error. One signal for the
    // three properties because they always move together — the dialog reads
    // all three on every change.
    void accountDeletionChanged();
    // The server wants the account password before it will delete. Emitted
    // once per challenge; the dialog shows its field on this.
    void accountDeletionPasswordRequired();
    // The account is gone. Sync has been stopped and the credential dropped;
    // ServerManager removes the server entry on this, because there is
    // nothing left on the other end to reconnect to.
    void accountDeactivated(const QString& serverUrl);

    // User-facing send feedback — emitted when the server rejects
    // a message with a useful-to-surface error (rate limits, size
    // caps, permission errors). QML subscribes and routes to
    // ToastHost. `kind` is "error" / "warning" / "info".
    void sendFeedback(const QString& text, const QString& kind);
    void roomTypingChanged();
    void roomPinnedEventsChanged(const QString& roomId);
    void profileFetched(const QString& userId, const QString& displayName, const QString& avatarUrl);
    // Answer to fetchNickname. `nickname` is empty when the user has none, which
    // is how an editor decides whether to offer "Remove nickname".
    void nicknameFetched(const QString& userId, const QString& nickname);
    // A nickname write landed. Emitted after the local caches are refreshed so a
    // listener re-reading them sees the new name.
    void nicknameChanged(const QString& userId, const QString& nickname);
    void myPowerLevelChanged();
    void serverRolesChanged();
    void permissionsChanged();
    // A bot flag was learned or changed. Coalesced to once per event-loop
    // turn — see flushBotFlagRepaint().
    void botFlagsChanged();
    void categorizedRoomsChanged();
    void bannedMembersChanged();
    void serverMembersChanged();
    // Fired when a state-event write (role assignment, channel override,
    // channel settings, server name) was rejected by the server. The QML
    // layer shows a toast and the optimistic local update is rolled back
    // before this signal fires. `kind` is a short tag ("role-assign",
    // "server-name", …) so the UI can tailor the message.
    //
    // On a dead session `kind` and `status` are still the real ones and only
    // `error` is replaced with the sign-in sentence — the emit site says why.
    void stateWriteFailed(const QString& kind, int status, const QString& error);
    // Emitted when something outside MessageView (e.g. a message-link click
    // from another server, or a cross-room jump) has asked the chat pane to
    // scroll to a specific event. MessageView listens and calls
    // positionViewAtIndex on the matching row, if loaded.
    void scrollToEventRequested(const QString& eventId);
    // Search results, already flattened for display. Each entry is
    // {eventId, roomId, roomName, sender, body, timestamp, msgtype}.
    // `appended` is true when this is a subsequent page and the UI should extend
    // rather than replace. `nextBatch` is "" when there are no further pages.
    // `total` is the server's full match count, which can exceed what's loaded.
    void searchResultsReady(const QVariantList& results, int total,
                            const QStringList& highlights,
                            const QString& nextBatch, bool appended);
    // A search could not be completed. `message` is already user-facing, and
    // on a dead session it is replaced wholesale — emit it through
    // emitSearchError, never directly.
    void searchErrored(const QString& message);
    // The effective notify level for `roomId` changed (either because the user
    // set it, or because the server's answer disagreed with the local cache).
    // QML rebinds the context-menu checkmarks on this.
    void notifyLevelChanged(const QString& roomId, const QString& level);
    // Server refused a notify-level write; the local value has already been
    // rolled back to what it was.
    //
    // main.qml toasts this: the rollback moves a checkmark inside a context
    // menu that has already closed, so without it the refusal is invisible
    // and the user is left believing the channel is muted. Emit it through
    // emitNotifyLevelFailed, never directly.
    void notifyLevelFailed(const QString& roomId, const QString& error);
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
    // The homeserver rejected our bearer token, from /sync or from any other
    // authenticated request. Idempotent: the ten in-flight requests that all
    // 401 at once raise one banner between them.
    void onAccessTokenRejected(const QString& errorBody);
    // Shared tail of every successful login (password, OIDC, re-auth):
    // install the new credential, clear the expired state, full-sync.
    void applyLoginResponse(const bsfchat::LoginResponse& response);
    // Arm (and first disarm) the one-shot handlers for a login reply.
    void awaitLoginReply();
    void onLoginAttemptFailed(const QString& error);
    // Return to a clearable expired state and tell the UI why.
    void failReauth(const QString& error);
    // `userId`'s latest membership value in every synced room, keyed by room id
    // (empty string where the room has no member event for them). The input to
    // bsfchat::client::moderationRooms(); split out so the three moderation
    // entry points cannot drift on how "latest" is computed.
    QMap<QString, QString> membershipByRoom(const QString& userId) const;
    // `userId`'s latest membership in ONE room ("join"/"invite"/"leave"/"ban",
    // or empty when no member event for them has ever been seen there).
    //
    // membershipByRoom() above answers the same question for every room at
    // once because the server-scope moderation helpers genuinely need all of
    // them. The invite dialog needs exactly one, on every keystroke, so it
    // gets its own lookup rather than building and throwing away a map of the
    // whole server each time.
    QString membershipInRoom(const QString& roomId, const QString& userId) const;
    // The channel's display name, for copy. Empty for a room this client has
    // not synced, which every message that uses it is written to survive.
    QString roomNameFor(const QString& roomId) const;

    MatrixClient* m_client;
    SyncLoop* m_syncLoop;
    // Persisted room-state + sync-token cache. Its only job is to let
    // startSync() resume incrementally instead of asking the server for a
    // full initial sync on every launch.
    LocalCache* m_cache;
    // True only while a cached snapshot is being replayed through
    // processSyncResponse(). Suppresses the two things in there that must
    // react to *server* syncs and not to a local replay: the first-sync
    // room prune, and writing the snapshot straight back to the cache.
    bool m_hydratingFromCache = false;
    // True once we have resumed from a persisted token, i.e. the first
    // server sync of this session is an incremental delta rather than the
    // whole world. Cleared if the token is later abandoned.
    bool m_resumedFromCache = false;
    // startSync() is reachable from four login paths; this keeps the cache
    // from being reopened and its signal reconnected if any of them run
    // twice in one session.
    bool m_cacheWired = false;
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
    // Incremented on every ticketReady. See the mediaTicketEpoch property.
    int m_mediaTicketEpoch = 0;
    // Downloads a media object with an Authorization header and hands the
    // resulting LOCAL FILE to the desktop. Created on first use — most sessions
    // never open a file externally. See openMediaExternally().
    MediaDownloader* m_mediaOpener = nullptr;
    QString m_avatarUrl;
    QString m_activeRoomId;
    QString m_activeRoomName;
    QString m_activeRoomTopic;
    bool m_connected = false;
    // 0=disconnected, 1=connected, 2=reconnecting, 3=session expired
    int m_connectionStatus = 0;
    // Where we stand with the homeserver's opinion of our token, and what a
    // reconnect is allowed to do about it. See net/SessionAuth.h.
    bsfchat::client::SessionAuth m_auth;
    // Set when a login replaces the credential: the next startSync() must
    // not re-seed itself from the cached `since`, which belongs to the
    // session the server just retired.
    bool m_forceFullSync = false;
    QString m_syncErrorMessage;
    bool m_hasUnread = false;
    bool m_viewingVoiceRoom = false;

public:
    bool viewingVoiceRoom() const { return m_viewingVoiceRoom; }

    // Per-room member cache: roomId -> list of member events
    // Per-room member cache, collapsed to the latest event per user by
    // bsfchat::client::upsertMemberEvent() (util/MemberCache.h) so it stays
    // bounded by the roster size rather than growing per event (U-M16).
    QMap<QString, QVector<bsfchat::RoomEvent>> m_roomMembers;
    QMap<QString, QStringList> m_roomPinnedEvents;
    // Whether the timeline view last reported itself parked at the end.
    // Starts true so a room opened and left without the view ever
    // reporting (no rows, model never populated) behaves as before.
    bool m_timelineAtBottom = true;
    // Newest ts whose read marker we have already POSTed, per room.
    // markRoomRead is called on every count change while the user sits at
    // the bottom of a busy channel; without this it would be one HTTP
    // request per inbound message.
    QHash<QString, qint64> m_sentReadMarkerTs;
    QMap<QString, qint64> m_userLastActivityMs; // for activity-based presence
    QString m_selfPresence = QStringLiteral("online");
    QString m_selfStatusMessage;
    // Authoritative presence + status messages from sync, keyed by
    // user_id. Beats the client-side "saw a message recently"
    // heuristic when the server actually relays presence (which it
    // does as of the PresenceHandler addition).
    QMap<QString, QString> m_userPresenceFromSync;
    QMap<QString, QString> m_userStatusMessage;
    // DM store: roomId -> peer user id, plus the in-flight create guard.
    // Persisted via Settings under "dm/<server host>/<roomId>"; hydrated on
    // setup and extended by m.direct on sync.
    bsfchat::net::DirectRooms m_directRooms;
    void persistDirectRoom(const QString& roomId);
    // The one way a room becomes a DM. Flags it in RoomListModel (which is
    // what keeps it out of this server's channels) and, when the peer is
    // known, records and persists it. `peer` may be empty: the isolation half
    // must not wait on knowing who.
    void recordDirectRoom(const QString& roomId, const QString& peer);
    // Classify from the room's own m.room.member state, which carries
    // `is_direct` for both participants. Independent of m.direct, so a client
    // that never received that account-data event still gets it right.
    void noteDirectMembership(const QString& roomId, const bsfchat::RoomEvent& event);
#ifdef BSFCHAT_VOICE_ENABLED
    // Per-peer video surfaces (RTP + legacy JPEG unified) — replaces
    // the old per-frame base64 data-URL maps.
    VideoStreamRegistry* m_videoRegistry = nullptr;
#endif
    // Peers whose polled voice-member row carries screen_sharing /
    // camera_on = true. Rebuilt on every voice/members result (and
    // the join response) by reconcileAnnouncedMedia(); self excluded.
    QSet<QString> m_announcedScreenSharers;
    QSet<QString> m_announcedCameraUsers;
    QMap<QString, float> m_peerLevels;
    int m_maxScreenShareQuality = 3; // 3 = no limit by default
    // -1 sentinels = "no cap, honour user setting verbatim"
    int m_maxScreenShareFps   = -1;
    int m_maxScreenShareWidth = -1;
    int m_maxScreenShareJpeg  = -1;
    int m_maxScreenShareBitrate = -1;
    bool m_allowLossless = true;
    // Global userId → display name, populated from all m.room.member events
    // across every room. MessageModel reads from this pointer.
    QMap<QString, QString> m_userDisplayNames;

    // ── Bot identity state ────────────────────────────────────────────────
    //
    // Every user id this connection has seen a `bsfchat.bot` on, populated
    // from m.room.member content across every room — the direct analogue of
    // m_userDisplayNames above, and fed from the same places. MessageModel
    // reads it through a pointer; MemberListModel does not need it, because
    // the flag is on the member event that builds its rows.
    //
    // A SET rather than a map, because the server's encoding is set-or-erase:
    // `bsfchat.bot` is written only for bots and never as `false`, so absence
    // is the complete and only representation of "human". There is no third
    // state, nothing to probe for, and nothing that arrives late.
    QSet<QString> m_botUserIds;
    BotAdminModel* m_botAdminModel = nullptr;
    SelfRoleModel* m_selfRoleModel = nullptr;
    ChannelInviteModel* m_channelInviteModel = nullptr;
    BlockedUsersModel* m_blockedUsersModel = nullptr;
    // The account-deletion handshake. Owned here rather than by the dialog so
    // a dialog closed mid-flight cannot leave a half-completed UIA session
    // that the next attempt inherits.
    bsfchat::net::DeactivateFlow m_deactivate;
    // The password for the stage in flight, held only between
    // submitAccountDeletionPassword() and the reply it produces. Cleared on
    // every terminal outcome — it is the account's real credential and there
    // is no reason for it to outlive the one request it was typed for.
    QString m_deactivatePassword;
    // Push the current role document and OUR OWN member.roles into the self-role
    // picker. Hung off this class's own serverRolesChanged() rather than called
    // from each write path, because all four of them (a server.roles event, a
    // member.roles event, an optimistic setMemberRoles, its rollback) already
    // emit it — and the one that gets forgotten later is the one that would
    // leave the picker showing a role that no longer exists.
    void refreshSelfRoles();
    int m_botFlagsGeneration = 0;
    // Fold one answer into the set and repaint if it moved. Takes the flag as
    // read off a member event, a profile reply or /whoami — all three carry
    // the same key with the same meaning.
    void recordBotFlag(const QString& userId, bool isBot);
    void loadMembersForRoom(const QString& roomId);
    // Start a history fill of `kind` in the active room and send its first
    // request, if the model agrees to start one (see beginHistoryFill).
    void requestHistoryFill(bsfchat::client::HistoryFillKind kind, int firstPageLimit);

    // Identity (OIDC)
    IdentityClient* m_identityClient = nullptr;
    // The provider this connection was restored with, from the saved
    // server entry. Only read when there is no live m_identityClient,
    // i.e. on any launch where the OIDC flow hasn't run in-process.
    QString m_storedIdentityProviderUrl;

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

    // In-flight search state. `m_searchTerm` is the most recently REQUESTED
    // term; a response naming anything else is stale and dropped (the search box
    // fires per keystroke and replies can complete out of order).
    QString m_searchTerm;
    // @mxid → display name for a search hit, falling back to the localpart.
    // Search spans rooms, so MemberListModel (active room only) can't serve it.
    QString displayNameForSender(const QString& userId) const;

    // roomId → notify level as it was before an in-flight PUT, so a rejected
    // write can be undone instead of leaving the local cache claiming a
    // preference the server refused.
    QMap<QString, QString> m_pendingNotifyLevelRollback;

    // Voice state
    // Mirrors of VoiceSession state, kept for the Q_PROPERTYs QML binds
    // to. The session is the authority; these follow it.
    QString m_activeVoiceRoomId;
    QString m_voiceError;
    // The join/leave state machine (V-C1/V-H1/V-H2/V-M4). Replaced the
    // `m_voiceTurnFetchPending` bool that used to be the join's only
    // guard; read src/net/VoiceSession.h for what that bool could not do.
    VoiceSession* m_voiceSession = nullptr;
    // Wire the session to MatrixClient, to the engine, and to the mirrors
    // above. Called once from the constructor.
    void setupVoiceSession();
    // The session's engine-start hook. Builds a VoiceEngine, wires it and
    // starts it; false means the session must unwind the join (V-H4).
    bool startVoiceEngine(const QString& roomId, const QJsonArray& members,
                          const QJsonObject& config);
    // Hand one inbound m.call.* event to the running engine. Called by
    // the session, both live and when replaying what it buffered during
    // the join (V-C1).
    void dispatchCallSignal(const CallSignal& signal);
    // The OS microphone permission, flattened for voice::micPermissionAction.
    // Always Unsupported on a platform (or a Qt) without permissions. V-H4.
    voice::MicPermission microphonePermission() const;
    // Applies that rule before voice/join on platforms that gate the
    // microphone (macOS/iOS/Android, Qt 6.5+): fires the first-run
    // request when the user has not been asked, and returns FALSE when
    // the OS has already refused — a join that proceeds then is a member
    // nobody can hear. The refusal message is set for the UI. V-H4.
    bool microphonePermissionAllowsJoin();
    QTimer* m_voiceErrorTimer = nullptr;
    float m_micLevel = 0.0f;
    bool m_micSilent = false;
    int m_zeroLevelFrames = 0; // consecutive frames with near-zero level
    bool m_voiceMuted = false;
    bool m_pttPressed = false;
    Settings* m_settings = nullptr;
    void applyMicGate();  // recompute mute from voiceMuted + ptt mode
    // Synchronous, idempotent teardown of the entire local voice
    // session: engine, membership caches, timers, cached peer frames,
    // mute/deafen/mic state. Every leave/switch/kick path funnels
    // through here so no path can leak a running engine.
    void teardownVoiceSession();
    // Rebuild the announced-sharer sets from m_voiceMembers. Flag
    // flips emit peerScreenFrameChanged / peerCameraFrameChanged so
    // QML re-derives its tile lists; a flag dropping also erases the
    // peer's cached frame so a frozen last frame can't linger.
    void reconcileAnnouncedMedia();
    void setVoiceError(const QString& message);
    void clearVoiceError();

    // The ONLY place `sendFeedback` is emitted from. Do not emit it directly.
    //
    // sendFeedback drives one toast with several callers — the composer, the
    // nickname editor, channel and DM creation, message deletion — and every
    // one of them can fail with a 401 carrying a raw Matrix error object. The
    // guard used to live in the composer's handler alone, so the other four
    // printed {"errcode":"M_UNKNOWN_TOKEN",...} underneath the "session has
    // expired" banner: the 2026-09-19 presentation reached by renaming
    // yourself instead of joining voice. A rule every caller must remember is
    // a rule the next caller will not, so it is enforced here instead, the way
    // setVoiceError above already does it for the voice surface.
    void emitFeedback(const QString& text, const QString& kind);
    // Every reply to POST /account/deactivate, 401 included. Hands it to
    // m_deactivate and acts on the verdict. A member rather than a lambda
    // because the success branch tears this connection down, which is too much
    // to read inside a constructor.
    void onDeactivateResponse(int status, const QJsonObject& body);

    // The ONLY place `searchErrored` is emitted from. Do not emit it directly.
    //
    // Same guard, a different sentence: this text lands inline where the
    // results go, not in a toast, so it says why the list is empty rather
    // than repeating the banner above it verbatim. The .cpp has the reasoning
    // and the reason the signal must still fire (the popup's spinner is
    // cleared by this handler and nothing else).
    void emitSearchError(const QString& message);

    // The ONLY place `notifyLevelFailed` is emitted from. Guarded like the
    // rest since it acquired a receiver (main.qml's toast); the emit site
    // carries the reason it has one.
    void emitNotifyLevelFailed(const QString& roomId, const QString& error);

    // The ONLY place `mediaSendFailed` is emitted from for a failed REQUEST.
    // The two pre-flight checks in sendMediaMessage — no active room, file
    // won't open — do not come through here on purpose, because their text is
    // about something signing in again will not fix.
    void emitMediaSendFailed(const QString& error);

    // The ONLY place `mediaSendFailed` is emitted from for a failed PRE-FLIGHT
    // check: it keeps the caller's text and defers the emit to the next turn
    // of the event loop. That deferral is load-bearing — a pre-flight failure
    // reported synchronously arrives before the caller has counted the upload
    // it just started, and the composer never unlocks. The .cpp has the whole
    // account; the short version is that sendMediaMessage must never emit
    // mediaSendFailed before it returns.
    void emitPreflightMediaFailure(const QString& error);

    // The ONLY place `avatarUploadFailed` is emitted from. Same 401 guard as
    // emitMediaSendFailed, on its own signal: both avatar call sites are
    // reply handlers for a request that did go out, so both can carry a 401
    // body that wants the session sentence instead. The reason it is a
    // separate signal rather than a fourth caller of emitMediaSendFailed is
    // in the .cpp, and it is about the counter, not about the wording.
    void emitAvatarUploadFailed(const QString& error);
public:
    // Called once at startup by ServerManager so the mic gate can
    // consult voiceMode / PTT prefs without a global singleton.
    void setSettings(Settings* settings);
private:
    // Media-protection state of the live session. `m_voiceProtection` is
    // only meaningful while `m_voiceProtectionActive` is true; the two
    // getters return empty strings otherwise. Set exactly where a
    // transport starts, cleared exactly where one stops, so the badge
    // cannot outlive the connection it describes.
    voice::MediaProtection m_voiceProtection = voice::MediaProtection::SfuNone;
    bool m_voiceProtectionActive = false;
    void setVoiceProtection(voice::MediaProtection p);
    void clearVoiceProtection();
    bool m_voiceDeafened = false;
    QJsonArray m_voiceMembers;
    QTimer* m_voicePollTimer = nullptr;
    // Fold one m.call.member state event into the per-room voice roster the
    // sidebar renders. Sync is the ONLY writer of that roster: room state is
    // the server's own answer to "who is in this channel", it arrives for
    // every voice channel (not just the one we are in), and it cannot race
    // the /voice/members poll because the poll no longer touches it.
    void applyCallMemberEvent(const QString& roomId,
                              const bsfchat::RoomEvent& event);
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

    // Same diff-gated publish as rebuildCategorizedRooms(), for the other
    // snapshot lists QML rebuilds delegates from (see net/SnapshotGate.h).
    //
    // directRooms() stays a fresh build for its callers; m_publishedDirectRooms
    // only remembers what the last directRoomsChanged() announced, so a sync
    // pass that moved no DM's order, typing or presence stays silent.
    QVariantList m_publishedDirectRooms;
    void refreshDirectRooms();

    // serverMembers / bannedMembers walk every cached member event, so they
    // are rebuilt only when an input moved (m_roomMembers or the display-name
    // cache — mark the flag wherever either is written) and announced only
    // when the rebuilt list differs.
    QVariantList m_serverMembers;
    QVariantList m_bannedMembers;
    bool m_memberSnapshotsDirty = false;
    QVariantList buildServerMembers() const;
    QVariantList buildBannedMembers() const;
    void refreshMemberSnapshots();

    // Build the QML-facing voice roster snapshot (display names + media
    // flags + per-peer connection state stamped onto m_voiceMembers).
    QJsonArray buildVoiceMembers() const;
    // Publish that snapshot, emitting voiceMembersChanged() only when it
    // differs from the last published one (U-M9). `force` emits regardless,
    // for the join/leave transitions where the UI must resync either way.
    void emitVoiceMembersIfChanged(bool force = false);
    // The snapshot as of the last voiceMembersChanged() we emitted.
    QJsonArray m_lastEmittedVoiceMembers;

    // Typed permission caches, populated from sync state events.
    struct RoleInfo {
        QString id;
        QString name;
        QString color;
        int position = 0;
        quint64 permissions = 0;
        bool mentionable = false;
        bool hoist = false;
        // "Any member may add this role to themselves, and remove it again."
        // Mirrored here so the UI can tell an opt-in role apart from one an
        // admin grants; the server is the authority on whether a claim is
        // allowed (PUT/DELETE /bsfchat/self_roles/{id}), and a client must not
        // treat this bit as proof the role is harmless — see
        // protocol ServerRole for the containment rule that actually makes it so.
        bool selfAssignable = false;
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
    // A restore asked for a channel the room list didn't have yet. Re-tried at
    // the end of every sync until the list can answer.
    bool m_pendingRoomRestore = false;
    QSet<QString> m_knownRoomIds;
};
