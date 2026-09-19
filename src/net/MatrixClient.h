#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <bsfchat/MatrixTypes.h>

#include "net/MediaTicketCache.h"
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
    // `requestId` correlates the call with its createRoomSuccess/
    // createRoomError reply. Those signals are connection-wide and several
    // flows wait on them at once (a DM and a channel, two DMs), so without a
    // token the first reply was delivered to every waiting handler. See
    // net/TokenedReply.h.
    void createRoom(const QString& requestId, const QString& name, const QString& topic,
                    const QString& visibility = "private");

    // Create a direct-message room with a single other user:
    // trusted_private_chat preset, no name/topic, is_direct=true,
    // invitee on creation. Mirrors the Matrix spec DM convention.
    void createDirectMessageRoom(const QString& requestId, const QString& targetUserId);

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
    // Same PUT, but the outcome is reported against `token` through
    // callEventSendResult instead of the shared messageSent/
    // sendMessageError pair. Voice signalling needs to know whether ITS
    // event landed so it can retry (V-M2); the chat signals cannot say.
    void sendCallEvent(const QString& roomId, const QString& eventType,
                       const QByteArray& content, quint64 token);
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
    // `uploadId` correlates this upload with its mediaUploaded/
    // mediaUploadError reply. Several unrelated flows (attachment, user
    // avatar, server avatar) wait on those signals at once; without a token
    // the first completion was delivered to all of them. See
    // net/TokenedReply.h.
    void uploadMedia(const QString& uploadId, const QByteArray& data,
                     const QString& contentType, const QString& filename);

    // The URL to hand an Image/MediaPlayer for an mxc URI, or "" when there is
    // not yet a ticket for it.
    //
    // It used to return a URL with the session token in the query string; it
    // now returns one with a short-lived signed ticket, or nothing at all while
    // one is being fetched. Callers must treat "" as "not yet" and repaint when
    // MediaTicketCache::ticketReady fires — every existing one already did,
    // because this could always return "" before login. See util/MediaUrl.h.
    QString mediaDownloadUrl(const QString& mxcUri) const;

    // The mint-and-cache layer behind mediaDownloadUrl(). Exposed so the pieces
    // that resolve media without going through this class — MessageModel, which
    // bakes a URL into each row — share one cache and one set of tickets rather
    // than minting their own.
    bsfchat::client::MediaTicketCache* mediaTickets() { return &m_mediaTickets; }
    const bsfchat::client::MediaTicketCache* mediaTickets() const { return &m_mediaTickets; }

    // GET /account/whoami — the server's canonical identity for our
    // access token. Used to reconcile a persisted (possibly stale or
    // corrupt) user id on session restore. Silent on failure so old
    // servers without the endpoint keep working.
    void whoami();

    // Profile
    void getProfile(const QString& userId);

    // ── Bot accounts (BSFChat extension) ──────────────────────────────────
    //
    // All four are gated server-side on MANAGE_BOTS at SERVER scope. The
    // client gates the UI on the same flag (ServerConnection::canManageBots)
    // so the buttons are not offered to someone who would only get a 403, but
    // the server is the enforcer and these calls report its refusal rather
    // than assuming it cannot happen.
    //
    // THE TOKEN IS RETURNED EXACTLY ONCE, by createBot and rotateBotToken.
    // The server keeps no retrievable copy. Neither of those two paths logs
    // its response — not the body, not the status line, not a "created X"
    // trace — and neither may ever start to: the client mirrors every log
    // line into a rotating file on disk (util/FileLogger.h), so a debug print
    // here is a credential written to the user's home directory. The token
    // travels from the reply straight into BotAdminModel's one-time banner
    // and is dropped when the operator dismisses it.
    void listBots();
    void createBot(const QString& localpart, const QString& displayName,
                   const QString& description);
    void rotateBotToken(const QString& userId);
    // Deactivate, not delete. Idempotent server-side: deactivating an already
    // deactivated bot succeeds, so a double-click cannot produce an error the
    // operator has to interpret.
    void deactivateBot(const QString& userId);
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
    // `requestId` correlates the call with redactSucceeded/redactFailed.
    // Pass an empty id for a fire-and-forget redaction (reaction toggles).
    void redactEvent(const QString& requestId, const QString& roomId,
                     const QString& eventId, const QString& reason = {});
    void kickUser(const QString& roomId, const QString& userId, const QString& reason = {});
    void banUser(const QString& roomId, const QString& userId, const QString& reason = {});
    // Reverses a ban on `userId` in `roomId`. The user goes back to "leave"
    // state and can be re-invited / rejoin like anyone else.
    void unbanUser(const QString& roomId, const QString& userId);

    // Voice
    void joinVoice(const QString& roomId);
    // `sessionId` is the opaque token the join response returned for this
    // membership (empty against a server that does not issue one). The
    // server refuses to act on a token its stored row has moved past, so
    // echoing it is what stops a late leave from cancelling a FRESH join
    // — the server half of V-H1.
    void leaveVoice(const QString& roomId, const QString& sessionId = QString());
    void getVoiceMembers(const QString& roomId);
    void updateVoiceState(const QString& roomId, bool muted, bool deafened,
                          const QString& sessionId = QString());
    // Media-flag-only PUT to the same voice/state endpoint. The server
    // leaves any omitted key unchanged, so sending just the two media
    // flags can't clobber a mute/deafen toggle racing in from
    // updateVoiceState().
    void updateVoiceMediaState(const QString& roomId, bool screenSharing,
                               bool cameraOn,
                               const QString& sessionId = QString());
    void createVoiceChannel(const QString& name);
    void getTurnConfig();

    // Categories & Channels
    void createCategoryRoom(const QString& name);
    // Create a text or voice channel. Channels are always created public so
    // they auto-join to everyone — privacy is later enforced by a per-channel
    // @everyone DENY VIEW_CHANNEL override, applied separately by the caller
    // listening on createRoomSuccess.
    void createChannelInCategory(const QString& requestId, const QString& name,
                                 const QString& categoryId, bool isVoice = false);
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

    // Answers to redactEvent. It used to answer nothing at all: the reply was
    // deleteLater'd and dropped, so a redaction the server refused (403 after
    // a demotion, or an offline client) was indistinguishable from one it
    // accepted.
    void redactSucceeded(const QString& requestId, const QString& eventId);
    void redactFailed(const QString& requestId, const QString& error);
    void createRoomSuccess(const QString& requestId, const QString& roomId);
    void createRoomError(const QString& requestId, const QString& error);

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

    void mediaUploaded(const QString& uploadId, const QString& contentUri);
    // Per-upload progress 0..1. QNetworkAccessManager re-uses the
    // same reply object until it finishes, so `filename` identifies
    // which upload the tick belongs to when several run at once.
    void mediaUploadProgress(const QString& filename, double progress);
    void mediaUploadError(const QString& uploadId, const QString& error);

    void voiceJoined(const QString& roomId, const QJsonArray& members);
    // Same reply, with the server's session token for this membership.
    // Emitted immediately after voiceJoined. `sessionId` is empty against
    // a server that predates the token.
    void voiceJoinedSession(const QString& roomId, const QJsonArray& members,
                            const QString& sessionId, qint64 joinedAt);
    void voiceLeft(const QString& roomId);
    void voiceMembersResult(const QString& roomId, const QJsonArray& members);
    // Generic voice failure, kept for the user-facing toast: every voice
    // endpoint still reports through it. It is deliberately NOT the input
    // to the join/leave state machine — attributing failures by "a voice
    // request failed while a join was in flight" is precisely V-M4, where
    // the previous room's leave error unwound the new room's join.
    void voiceError(const QString& error);
    // Per-endpoint outcomes. Each carries the room it concerns so
    // VoiceSession can match a reply to the request it issued and ignore
    // replies to superseded ones. Emitted ALONGSIDE voiceError on
    // failure, never instead of it.
    void voiceJoinError(const QString& roomId, const QString& error);
    void voiceLeaveError(const QString& roomId, const QString& error);
    void voiceStateUpdated(const QString& roomId);
    void voiceStateError(const QString& roomId, const QString& error);
    // The server rejected a state PUT because our token is not the one it
    // holds (HTTP 403, "Voice session superseded") — our membership was
    // reaped or replaced. Recovery is re-POSTing voice/join, never
    // another state PUT: a reaped row is never re-activated by one.
    void voiceStateSuperseded(const QString& roomId);
    void voiceMembersError(const QString& roomId, const QString& error);
    void voiceChannelCreated(const QString& roomId);

    void callEventSendResult(quint64 token, bool ok, const QString& error);

    void turnConfigResult(const QJsonObject& config);
    // V-M4: TURN has its own failure channel, so an unrelated voice error
    // landing inside the TURN round trip can no longer unwind a join.
    void turnConfigError(const QString& error);

    // Canonical user id for our token, from GET /account/whoami.
    // Never fired on error (older servers 404 the endpoint).
    //
    // `isBot` is the response's `bsfchat.bot`, which the server reports for
    // the caller. It is false on a server that predates bots, and false for
    // every human, so it is safe to read unconditionally. Carried here rather
    // than left to a profile fetch because this is the one identity the
    // client always has an answer for without asking a second time.
    void whoamiResult(const QString& userId, bool isBot);

    // `isBot` is the profile's `bsfchat.bot` flag. It is a TRI-STATE on the
    // wire and only two states here, on purpose: a server that does not know
    // about bots omits the key, and a client that treated "absent" as
    // anything but false would badge every user on an older server. Absent,
    // false and a non-boolean all arrive here as false.
    void profileResult(const QString& userId, const QString& displayName,
                       const QString& avatarUrl, bool isBot);

    // ── Bot administration replies ────────────────────────────────────────
    //
    // `bots` is the raw array from GET /bsfchat/bots.
    void botsListed(const QJsonArray& bots);
    // A bot was created. `token` is the ONLY copy that will ever exist — see
    // the note on createBot. Nothing on the receiving side may store it.
    void botCreated(const QString& userId, const QString& displayName,
                    const QString& token);
    // A rotation succeeded; the bot's previous token is now dead. Same
    // one-copy rule as botCreated.
    void botTokenRotated(const QString& userId, const QString& token);
    void botDeactivated(const QString& userId);
    // `operation` is "list" / "create" / "rotate" / "deactivate" so the UI can
    // say which control failed; `status` is the HTTP status (0 for a
    // transport failure) and `error` the server's decoded message.
    void botRequestFailed(const QString& operation, int status,
                          const QString& error);

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

    // The homeserver answered 401 with a dead-token errcode to a request we
    // sent WITH a bearer header. Emitted once per such reply from whichever
    // endpoint saw it first, so the connection learns its session is over
    // from any request rather than only from /sync.
    //
    // Before this, the 401 handling hung off SyncLoop alone. Every other
    // subsystem kept driving the dead token and reported the rejection on
    // its own surface — in the 2026-09-19 purge the first thing the user
    // saw was a raw {"errcode":"M_UNKNOWN_TOKEN",...} toast from the voice
    // join path, not a prompt to sign in.
    void accessTokenRejected(const QString& errorBody);

private:
    QNetworkReply* makeRequest(const QString& method, const QString& path,
                                const QByteArray& body = {});
    QUrl buildUrl(const QString& path) const;
    // Watch one authenticated reply for a 401 that says our token is dead.
    //
    // Connected BEFORE the caller's own finished handler, and reads the body
    // with peek() rather than readAll() precisely because of that ordering —
    // readAll here would drain the buffer and hand every existing call site
    // an empty response. Call it only for requests that actually carried an
    // Authorization header; a 401 from /login is a wrong password, not a
    // revoked session.
    void watchForTokenRejection(QNetworkReply* reply);
    // Writes the MSC3952 `m.mentions` block into `content` (no-op for an empty
    // list). Shared by the plain-send, reply and edit paths — see the
    // definition for why the @room sentinel is handled here and not by callers.
    static void applyMentions(nlohmann::json& content,
                               const QStringList& mentionedUserIds);

    // POST /_matrix/media/v3/ticket for one object, feeding the reply back
    // into m_mediaTickets. Driven by MediaTicketCache::mintRequested.
    void requestMediaTicket(const QString& mxcUri);

    QNetworkAccessManager m_nam;
    bsfchat::client::MediaTicketCache m_mediaTickets;
    QString m_homeserver;
    QString m_accessToken;
};
