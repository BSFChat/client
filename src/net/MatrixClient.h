#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <bsfchat/MatrixTypes.h>

#include "net/MatrixFailure.h"
#include "net/MediaTicketCache.h"
#include "util/SearchParser.h"

class MatrixClient : public QObject {
    Q_OBJECT

public:
    explicit MatrixClient(QObject* parent = nullptr);
    // Tears the outstanding /sync down while this object is still whole,
    // rather than leaving it for ~QNetworkAccessManager to finish during
    // member destruction. See m_syncReply.
    ~MatrixClient() override;

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
    //
    // At most ONE /sync may be outstanding on a client at a time, and this
    // enforces it: a call made while a poll is still in flight abandons the
    // old one first. Two concurrent polls on one credential is never a thing
    // anybody wants — they each hold a socket (and, on the server, an httplib
    // worker for the socket's whole life), they answer into the same models,
    // and the slower one writes back a stream position the faster one has
    // already passed.
    void sync(const QString& since = {}, int timeout = 30000);

    // Abandon the outstanding /sync, if any, without reporting it as an
    // error. SyncLoop::stop() needs this: leaving the poll running meant a
    // stop()/start() pair inside the 30s window left the reply from before
    // the stop still in the air, and it arrived to find the loop running
    // again and scheduled a successor. That is a second, permanent poll
    // chain on one SyncLoop — see tests/test_sync_single_flight.cpp.
    void abortSync();

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
    // Returns the transaction id the send was issued under. That id is the
    // handle for the local echo: the caller puts a row on screen with it
    // immediately, and messageSendAccepted/messageSendFailed name it when
    // the server answers. It used to be a local variable that was minted,
    // used in the URL and thrown away, which is why nothing could correlate
    // a send with its outcome.
    QString sendMessage(const QString& roomId, const QString& body);
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
    QString sendRichMessage(const QString& roomId, const QString& body,
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

    // The member-facing half of self-assignable roles. Deliberately NOT a
    // member.roles state write: that path is MANAGE_ROLES plus rank and would
    // 403 for the ordinary member this exists for. These two go to
    // PUT/DELETE /_matrix/client/v3/bsfchat/self_roles/{roleId}, which asks
    // only that the role be flagged `self_assignable` and still inside
    // @everyone's permissions.
    //
    // Both are idempotent server-side — taking a role you hold, or dropping
    // one you do not, is a 200 that writes nothing — so a double click costs
    // a round trip and nothing else.
    void addSelfRole(const QString& roleId);
    void removeSelfRole(const QString& roleId);

    // ── Account data, and the block list it carries ───────────────────────
    //
    // GET/PUT /_matrix/client/v3/user/{userId}/account_data/{type}. `userId`
    // must be us — the server answers 403 for anybody else's, and there is no
    // permission that unlocks it, because the one document this client stores
    // there is the block list and its whole value is that its subjects cannot
    // read it.
    //
    // THE PUT IS A FULL REPLACEMENT. There is no add/remove verb; the body is
    // the complete new document and anything left out is gone. Callers must
    // build it from the last document the server gave them — see
    // net/IgnoredUsers.h, which is the only thing that should be constructing
    // one.
    //
    // A 404 is not an error: it is how the server says this account has never
    // written this document, and it arrives as accountDataAbsent rather than
    // accountDataError so a caller cannot mistake "no block list" for "the
    // request failed" and leave the UI empty for the wrong reason.
    //
    // There is NO account_data section in /sync on this server, so nothing
    // here ever arrives unasked. A change made on another device is invisible
    // until the next explicit GET.
    void getAccountData(const QString& userId, const QString& type);
    void putAccountData(const QString& requestId, const QString& userId,
                        const QString& type, const QJsonObject& content);

    // ── Reporting ─────────────────────────────────────────────────────────
    //
    // POST /rooms/{roomId}/report/{eventId} and POST /users/{userId}/report.
    // Both answer 200 {} and change NOTHING: server-side a report is one row
    // and one audit record for an administrator to act on later, by hand. It
    // does not redact, mute, hide or notify. Copy next to these calls must not
    // promise otherwise.
    //
    // `requestId` correlates the call with its reply, as it does for invites:
    // the dialog that is waiting is one of several things that can be on
    // screen, and a reply that lands after it was closed and reopened for a
    // different target must not be attributed to the new one.
    //
    // Both are rate limited per account (SendLimiter::Bucket::kReport), so a
    // 429 here is ordinary rather than exceptional and reportFailed carries
    // the wait the server asked for.
    void reportEvent(const QString& requestId, const QString& roomId,
                     const QString& eventId, const QString& reason);
    void reportUser(const QString& requestId, const QString& userId,
                    const QString& reason);

    // ── Account deletion ──────────────────────────────────────────────────
    //
    // POST /account/deactivate. Matrix user-interactive auth: the first call
    // sends no `auth` and the server answers either 200 (an account with no
    // password — one signed in through the identity provider) or 401 carrying
    // the m.login.password flows, which is answered by repeating the request
    // with `auth`.
    //
    // THE FIRST CALL IS NOT A PROBE. For an OIDC account it deletes the
    // account outright, so the user's confirmation must be complete before it
    // is made; the password prompt is a second gate that only some accounts
    // see, never the confirmation itself. net/DeactivateFlow.h owns that
    // distinction and is what should be driving these two.
    //
    // Pass an empty `auth` for the first call. The 401 is delivered as
    // deactivateResponse like any other status — deliberately NOT as an error
    // — because telling a UIA challenge apart from a dead access token is a
    // decision about the body, not the status, and it belongs in one place.
    void deactivateAccount(const QJsonObject& auth);

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
    // Add `userId` to `roomId` — POST /rooms/{roomId}/invite.
    //
    // Unlike kick/ban/unban below, which are fire-and-forget, this one reports
    // back: it is a construction, not a moderation act, and the person who
    // asked for it is sitting in front of a dialog waiting to find out whether
    // it worked. The reply comes back as inviteSucceeded / inviteFailed, both
    // of which echo the user id so a reply that lands after the field was
    // retyped is still attributed to the right target.
    void inviteUser(const QString& roomId, const QString& userId);
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
    // The same two outcomes, but naming WHICH send they belong to.
    //
    // messageSent/sendMessageError are connection-wide and anonymous: with
    // two sends in flight there is no way to tell which one a reply is
    // about, which is why nothing was ever connected to messageSent and why
    // sendMessageError could only ever raise a generic toast. These carry
    // the transaction id returned by sendMessage/sendRichMessage, so a
    // local echo can be reconciled or marked failed individually.
    void messageSendAccepted(const QString& localId, const QString& eventId);
    void messageSendFailed(const QString& localId, const QString& error);

    // roomId is carried alongside the response so the receiver can
    // filter to the currently-active room. The server doesn't echo
    // the roomId in the body, and without it the handler can apply
    // results to the wrong model when a room switch happens between
    // request and response (race on cold start, observed v0.0.34).
    void messagesResult(const QString& roomId,
                        const bsfchat::MessagesResponse& response);
    // `roomId` and `from` identify the request, as they do for a result:
    // without them a failure could not be matched to the history fill it
    // belongs to, and the fill stayed "loading" until the room was switched —
    // one failed page and scroll-up was dead for the rest of the visit.
    void messagesError(const QString& roomId, const QString& from, const QString& error);

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

    // ── Channel invite replies ────────────────────────────────────────────
    //
    // The server answers `{}` for every shape of the success case — a human
    // left at membership 'invite', a bot joined outright, and (since server
    // a19fd10) an already-joined target for whom nothing was written at all
    // — and there is no field that says which it did. So there is one signal,
    // and the copy claims only what is true of all three; see
    // ChannelInviteModel::noticeFor.
    void inviteSucceeded(const QString& roomId, const QString& userId);
    // `status` is the HTTP status (0 for a transport failure) and `error` the
    // server's decoded message. Both are needed: handle_invite answers six
    // different situations with 403 M_FORBIDDEN and the text is what tells
    // them apart.
    void inviteFailed(const QString& roomId, const QString& userId,
                      int status, const QString& error);

    // A self-role PUT/DELETE succeeded. `roleIds` is the server's own
    // post-change bsfchat.member.roles list, not an echo of what we asked
    // for — the picker adopts it verbatim so an idempotent no-op, or a change
    // another client made in the same window, corrects the UI instead of
    // being papered over by our optimism.
    void selfRoleChanged(const QString& roleId, const QStringList& roleIds);
    // A refusal. 403 is the ordinary one and means the containment re-check
    // failed: the role is still flagged self-assignable but its permissions
    // are no longer a subset of @everyone's.
    void selfRoleFailed(const QString& roleId, int status, const QString& error);

    // ── Account-data replies ──────────────────────────────────────────────
    //
    // `content` is the document verbatim. Nothing here interprets it: the one
    // type this client writes is parsed by net/IgnoredUsers.h, and a type it
    // does not know about must still round-trip byte-for-byte.
    void accountDataResult(const QString& type, const QJsonObject& content);
    // 404 — never written. See getAccountData for why this is not an error.
    void accountDataAbsent(const QString& type);
    void accountDataError(const QString& type,
                          const bsfchat::net::MatrixFailure& failure);
    // A PUT landed. `content` echoes what was STORED, so the caller adopts the
    // document the server now holds rather than its own idea of it.
    void accountDataStored(const QString& requestId, const QString& type,
                           const QJsonObject& content);
    void accountDataStoreError(const QString& requestId, const QString& type,
                               const bsfchat::net::MatrixFailure& failure);

    // ── Report replies ────────────────────────────────────────────────────
    //
    // Success carries nothing but the token: the server answers {} and there
    // is no report id to show, on purpose — a reference number would imply a
    // ticket the reporter can follow, and there is no such thing here.
    void reportFiled(const QString& requestId);
    // `failure` carries the wait for the 429 case, which is the ordinary
    // refusal on these two endpoints.
    void reportFailed(const QString& requestId,
                      const bsfchat::net::MatrixFailure& failure);

    // ── Deactivation reply ────────────────────────────────────────────────
    //
    // EVERY status the server answers arrives here, 401 included, with the
    // parsed body. net/DeactivateFlow decides what each one means; splitting
    // that decision across a success and an error signal is how a UIA
    // challenge and a dead session would eventually be confused for each
    // other. `body` is an empty object when the response did not parse.
    void deactivateResponse(int status, const QJsonObject& body);
    void deactivateTransportFailure(const QString& error);

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
    // The one reply handler both report endpoints share. See the definition
    // for why it is one function and not two.
    void wireReportReply(QNetworkReply* reply, const QString& requestId);

    // The outstanding /sync, or null. Identity, not ownership: it is the
    // token a finished handler compares itself against to find out whether
    // it is still the current poll. Cleared before the reply is abandoned,
    // so a superseded handler answers "not me" and returns silently instead
    // of emitting syncError for a cancellation nobody asked about.
    //
    // DECLARED BEFORE m_nam, so that it outlives the network manager:
    // members are destroyed in reverse declaration order, and a reply that
    // ~QNetworkAccessManager finishes on its way out would otherwise run
    // the handler below against a member that has already gone. The
    // destructor aborts the poll before any of that can happen, so this is
    // the second of two guards rather than the load-bearing one — but the
    // ordering costs nothing and the failure it prevents is silent.
    QPointer<QNetworkReply> m_syncReply;
    QNetworkAccessManager m_nam;
    bsfchat::client::MediaTicketCache m_mediaTickets;
    QString m_homeserver;
    QString m_accessToken;
};
