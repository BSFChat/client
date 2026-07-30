#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <bsfchat/MatrixTypes.h>

#include "util/SearchParser.h"

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

    // Create a direct-message room with a single other user:
    // trusted_private_chat preset, no name/topic, is_direct=true,
    // invitee on creation. Mirrors the Matrix spec DM convention.
    void createDirectMessageRoom(const QString& targetUserId);

    void joinRoom(const QString& roomIdOrAlias);
    void leaveRoom(const QString& roomId);
    void deleteRoom(const QString& roomId);
    void getJoinedRooms();
    void getRoomMembers(const QString& roomId);

    // Messages
    void sendMessage(const QString& roomId, const QString& body);
    // Rich message with explicit HTML formatting and @mention targeting.
    // `formattedBody` is the `format: org.matrix.custom.html` payload
    // (sender-generated — the composer adds <a> anchors for @Name and
    // #channel tokens). `mentionedUserIds` populates `m.mentions.user_ids`
    // so the server can elevate notifications for targeted users.
    //
    // An `@room` broadcast rides in as the literal entry kRoomMentionSentinel
    // in `mentionedUserIds`; it is lifted out into `m.mentions.room` (which is
    // a boolean, not a user id) rather than widening the signature, because
    // the composer reaches this through ServerConnection::sendRichMessage and
    // that file is being edited concurrently.
    static constexpr QLatin1StringView kRoomMentionSentinel{"@room"};
    void sendRichMessage(const QString& roomId, const QString& body,
                          const QString& formattedBody,
                          const QStringList& mentionedUserIds);
    void sendRoomEvent(const QString& roomId, const QString& eventType, const QByteArray& content);
    void getRoomMessages(const QString& roomId, const QString& from, const QString& dir = "b", int limit = 50);
    // Edit a previously-sent m.room.message by the current user. Sends a
    // new m.room.message with m.relates_to {rel_type: m.replace,
    // event_id: targetEventId}; the server accepts it only if sender matches.
    //
    // `formattedBody` / `mentionedUserIds` go into `m.new_content` and NOT the
    // top-level fallback, which is the "* edited text" plain-text stand-in.
    // They are carried for RENDERING, not for badging: the server refuses to
    // create or move mention rows on an m.replace (see
    // server/src/api/EventHandler.cpp — record_mentions is skipped for edits),
    // so adding an @name while editing will never fire a badge. What this does
    // fix is the reverse: the server folds an edit's m.new_content in as the
    // event's current content, so a payload that dropped m.mentions made the
    // original's highlight vanish for anyone loading the room fresh.
    void editMessage(const QString& roomId, const QString& targetEventId,
                      const QString& newBody,
                      const QString& formattedBody = QString(),
                      const QStringList& mentionedUserIds = {});
    // Send a new m.room.message with m.relates_to.m.in_reply_to.event_id
    // pointing at targetEventId. Server needs no special support — it's
    // just content metadata.
    //
    // A reply is an ordinary new message as far as mentions go, so
    // `m.mentions` here is fully honoured server-side: mention rows, badge and
    // push all fire exactly as they do for a plain send.
    void replyToMessage(const QString& roomId, const QString& body,
                         const QString& targetEventId,
                         const QString& formattedBody = QString(),
                         const QStringList& mentionedUserIds = {});
    // Send an m.thread relation — identical payload to a reply, but
    // with rel_type="m.thread" and event_id pointing at the thread
    // root. Clients that don't understand threads see it as a
    // standalone message (acceptable fallback).
    void sendThreadReply(const QString& roomId, const QString& body,
                          const QString& threadRootId);
    // Send an m.room.message with msgtype=m.emote (the /me command).
    void sendEmote(const QString& roomId, const QString& body);
    // Send an m.reaction event annotating targetEventId with `emoji`.
    // Server treats it as a normal event and distributes it through /sync;
    // the client aggregates state in MessageModel.
    void sendReaction(const QString& roomId, const QString& targetEventId,
                       const QString& emoji);
    // Redact a previously-sent reaction (to unreact). Thin wrapper around
    // redactEvent — exists so callers read naturally.
    void redactReaction(const QString& roomId, const QString& reactionEventId);
    // Send a new plain m.room.message into destRoomId, prefixed with a
    // "Forwarded from #source by @sender" attribution. No Matrix relation.
    // The source{ServerUrl,RoomId,EventId} trio lets us embed a
    // bsfchat://message/... link in the header so recipients can click
    // through to the original message & context.
    void forwardMessage(const QString& destRoomId, const QString& body,
                         const QString& sourceChannelName,
                         const QString& sourceSenderName,
                         const QString& sourceServerUrl = {},
                         const QString& sourceRoomId = {},
                         const QString& sourceEventId = {});

    // Media
    void uploadMedia(const QByteArray& data, const QString& contentType, const QString& filename);
    QString mediaDownloadUrl(const QString& mxcUri) const;

    // GET /account/whoami — the server's canonical identity for our
    // access token. Used to reconcile a persisted (possibly stale or
    // corrupt) user id on session restore. Silent on failure so old
    // servers without the endpoint keep working.
    void whoami();

    // Profile
    void getProfile(const QString& userId);
    void setDisplayName(const QString& userId, const QString& displayName);
    void setAvatarUrl(const QString& userId, const QString& avatarUrl);

    // Per-server nickname (BSFChat extension — Matrix has no nickname concept).
    // Unlike the calls above, `userId` is not necessarily us: the server gates
    // self-renames on CHANGE_NICKNAME and renames of others on MANAGE_NICKNAMES,
    // so this can legitimately fail with a 403 and the failure must be surfaced.
    void getNickname(const QString& userId);
    // An EMPTY `nickname` clears it, sent as JSON null. The server treats absent,
    // null and all-whitespace alike as "clear", so an emptied text field in the UI
    // is how a nickname is removed — there is no separate delete call.
    void setNickname(const QString& userId, const QString& nickname);

    // PUT /presence/{userId}/status — pushes presence + an optional
    // free-form status message. Matrix delivers this to other clients
    // in their next /sync's `presence` block.
    void putPresence(const QString& userId, const QString& presence,
                      const QString& statusMessage);

    // Typing
    void setTyping(const QString& roomId, const QString& userId, bool typing, int timeout = 5000);

    // Read marker (server-tracked unread counts).
    // Server marks everything currently in the room as read for this user.
    void sendReadMarker(const QString& roomId);

    // Full-text message search — POST /_matrix/client/v3/search.
    //
    // The server owns permission filtering (it only ever searches channels the
    // caller can VIEW_CHANNEL) and query sanitisation: it tokenises the input
    // and treats every FTS5 metacharacter as a separator, so a user typing `"`
    // or `*` cannot produce a syntax error — punctuation-only input just comes
    // back as zero matches. `searchFailed` therefore covers real failures
    // (unauthorised, no FTS5 module on the server, transport) rather than
    // "you typed a funny character".
    //
    // `nextBatch` is the opaque token from a previous page; empty for page one.
    void searchMessages(const QString& searchTerm, int limit,
                         const QString& nextBatch = QString());

    // ── Per-room notification level ───────────────────────────────────────
    //
    // GET/PUT /_matrix/client/v3/bsfchat/rooms/{roomId}/notify_level.
    // `level` is one of "all" / "mentions" / "none".
    //
    // Namespaced bsfchat.* rather than pretending to be Matrix push rules,
    // which is the whole reason the client has to speak this explicitly: the
    // setting used to live only in local QSettings, so the server could not
    // evaluate what to push for the user on any other device.
    void getRoomNotifyLevel(const QString& roomId);
    void setRoomNotifyLevel(const QString& roomId, const QString& level);

    // Permissions / roles
    void setMemberRoles(const QString& roomId, const QString& userId, const QStringList& roleIds);
    // targetKey is "role:<id>" or "user:<mxid>". Pass allow=0,deny=0 to clear.
    void setChannelPermission(const QString& roomId, const QString& targetKey,
                              quint64 allow, quint64 deny);
    void setChannelSlowmode(const QString& roomId, int seconds);
    void redactEvent(const QString& roomId, const QString& eventId, const QString& reason = {});
    void kickUser(const QString& roomId, const QString& userId, const QString& reason = {});
    void banUser(const QString& roomId, const QString& userId, const QString& reason = {});
    // Reverses a ban on `userId` in `roomId`. The user goes back to "leave"
    // state and can be re-invited / rejoin like anyone else.
    void unbanUser(const QString& roomId, const QString& userId);

    // Voice
    void joinVoice(const QString& roomId);
    void leaveVoice(const QString& roomId);
    void getVoiceMembers(const QString& roomId);
    void updateVoiceState(const QString& roomId, bool muted, bool deafened);
    // Media-flag-only PUT to the same voice/state endpoint. The server
    // leaves any omitted key unchanged, so sending just the two media
    // flags can't clobber a mute/deafen toggle racing in from
    // updateVoiceState().
    void updateVoiceMediaState(const QString& roomId, bool screenSharing, bool cameraOn);
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

    // roomId is carried alongside the response so the receiver can
    // filter to the currently-active room. The server doesn't echo
    // the roomId in the body, and without it the handler can apply
    // results to the wrong model when a room switch happens between
    // request and response (race on cold start, observed v0.0.34).
    void messagesResult(const QString& roomId,
                        const bsfchat::MessagesResponse& response);
    void messagesError(const QString& error);

    // Search results. `searchTerm` and `requestedNextBatch` echo the request so
    // the receiver can drop a stale response: the search box fires a request
    // per keystroke and replies can land out of order, and appending page 2 of
    // an abandoned query onto page 1 of the current one is worse than dropping
    // it. `nextBatch` (response-side) is empty when this was the last page.
    void searchResult(const QString& searchTerm, const QString& requestedNextBatch,
                      const bsfchat::client::SearchResponse& response);
    // Transport-level failure only — an error *payload* arrives via searchResult
    // with response.ok == false, so a single handler can render both.
    void searchFailed(const QString& searchTerm, const QString& error);

    // Per-room notify level read back from the server. `isDefault` is true when
    // the user has made no explicit choice and `level` is the server's default
    // for that room kind (DMs default to "all", channels to "mentions") — a
    // settings UI can then render "Default (mentions)" rather than a hard
    // selection the user never made.
    void roomNotifyLevelResult(const QString& roomId, const QString& level,
                               bool isDefault);
    // PUT rejected. The local preference is rolled back by the caller.
    void roomNotifyLevelError(const QString& roomId, const QString& error);

    void mediaUploaded(const QString& contentUri);
    // Per-upload progress 0..1. QNetworkAccessManager re-uses the
    // same reply object until it finishes, so `filename` identifies
    // which upload the tick belongs to when several run at once.
    void mediaUploadProgress(const QString& filename, double progress);
    void mediaUploadError(const QString& error);

    void voiceJoined(const QString& roomId, const QJsonArray& members);
    void voiceLeft(const QString& roomId);
    void voiceMembersResult(const QString& roomId, const QJsonArray& members);
    void voiceError(const QString& error);
    void voiceChannelCreated(const QString& roomId);

    void turnConfigResult(const QJsonObject& config);

    // Canonical user id for our token, from GET /account/whoami.
    // Never fired on error (older servers 404 the endpoint).
    void whoamiResult(const QString& userId);

    void profileResult(const QString& userId, const QString& displayName, const QString& avatarUrl);

    // Per-server nickname read-back. `nickname` is empty when the user has none —
    // the endpoint omits the key entirely rather than returning "", so empty here
    // unambiguously means "no nickname set".
    void nicknameResult(const QString& userId, const QString& nickname);
    // A nickname write succeeded. `nickname` is empty when it was cleared. The
    // resulting member events arrive via /sync like any other profile change, so
    // listeners use this only to refresh an open editor, not to update caches.
    void nicknameUpdated(const QString& userId, const QString& nickname);
    // A nickname write was rejected. Carries the decoded status + message so the
    // UI can distinguish "your role may not do this" (403) from a rejected value
    // (400) instead of failing silently, which is how the permission-gated write
    // would otherwise look identical to success.
    void nicknameError(const QString& userId, int status, const QString& error);

    // Fired whenever PUT /rooms/{id}/state/{type}/{key} fails. The UI uses
    // this to (a) surface a toast and (b) roll back any optimistic local
    // updates it applied in anticipation of success. `stateKey` is the
    // state_key of the attempted write; `status` is the HTTP status (0 if
    // the network request itself failed); `error` is the human-readable
    // message decoded from the response body (or Qt's network error).
    void stateEventError(const QString& roomId, const QString& eventType,
                         const QString& stateKey, int status, const QString& error);

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
    // Writes the MSC3952 `m.mentions` block into `content` (no-op for an empty
    // list). Shared by the plain-send, reply and edit paths — see the
    // definition for why the @room sentinel is handled here and not by callers.
    static void applyMentions(nlohmann::json& content,
                               const QStringList& mentionedUserIds);

    QNetworkAccessManager m_nam;
    QString m_homeserver;
    QString m_accessToken;
};
