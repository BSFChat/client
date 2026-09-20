#pragma once

#include <QObject>
#include <QString>

#include <functional>

// View-model behind "add someone to this channel" — the client half of
// POST /_matrix/client/v3/rooms/{roomId}/invite.
//
// ─────────────────────── why this exists at all ───────────────────────
//
// The endpoint has worked since bots landed. The client had no caller: a grep
// for "invite" across qml/ found comments and voice-call signalling and
// nothing else, and ChannelList.qml carried a note saying invite-based joining
// was a future feature. So the bot documentation's first onboarding step —
// "invite the bot to a channel, which joins it outright" — could not be
// performed from the product at all.
//
// On 2026-09-20 the workaround was: SSH to the production host, open the
// server's SQLite database, read the room id out of it by hand, and curl the
// join endpoint with the bot's access token. That is a reasonable thing for a
// developer to do once and an unreasonable thing to ask of a self-hoster ever,
// which is the whole reason this file is here.
//
// ──────────────────── why a view-model and not QML ────────────────────
//
// Same reason BotAdminModel gives: everything interesting here is a decision
// about what to say when something goes wrong, ServerConnection cannot be
// instantiated in a unit test, and a message that only exists inside a QML
// binding is a message nothing headless can read back. It reaches the network
// through `Hooks` (std::function), the seam BotAdminModel and VoiceSession
// both use, so tests/test_channel_invite.cpp drives the whole thing with a
// recording fake.
//
// ───────────────── what the server does, which drives all of this ─────
//
// From server/src/api/RoomHandler.cpp handle_invite(), in order:
//
//   * the CALLER must be a member of the room                 403
//   * the room must not be a DM                               403
//   * the caller must hold MANAGE_CHANNELS **in that room**   403
//   * the target must not be banned in the room or server-wide 403
//   * the target must be an account that EXISTS            403
//   * a BOT target joins immediately — membership 'join', a real join event,
//     and an audit record naming the INVITER — because a bot has no human to
//     accept an invite and cannot even see one (SyncResponse has no `invite`
//     section). A second invite for a bot already in the room is a silent,
//     writeless 200.
//   * a deactivated bot is refused                            403
//   * a HUMAN target gets membership 'invite' and can then join.
//   * a target who is ALREADY JOINED is a writeless 200, for a human exactly
//     as for a bot. It used to rewrite their membership from 'join' back to
//     'invite' — demoting a member nobody asked to demote and taking them out
//     of everyone else's roster until they joined again — so this class
//     refused that case locally off the cached roster. Server a19fd10
//     (fix/invite-no-demote) closed it (handle_invite and the generic
//     PUT .../state/m.room.member/{userId} route both), the local refusal is
//     gone with it, and invite() now always reaches the server. See the note
//     in invite() for why reinstating it would make things worse, not safer.
//
// ─────────────── a second gap, closed on the server 2026-09-20 ────────────
//
// handle_invite USED TO ACCEPT AN INVITE FOR AN ACCOUNT THAT DID NOT EXIST.
// It never called user_exists(), and room_members.user_id has no foreign key,
// so inviting @tpyo:bsfchat.com returned 200 and left a membership row behind
// for nobody — visible in the roster with no name, and announced to the
// channel as an arrival. The operator's only evidence was that the person
// they meant never turned up.
//
// The client could not detect it, so it did the best it could: userIdError()
// for structure, and a homeserverWarning() that guessed a domain mismatch was
// probably a typo. Server b8e26ac closed the gap with a 403 that says so, so
// the guess is gone and explainFailure answers it from the server's own
// refusal. userIdError() stays — it is structure, not a prediction, and it
// still saves the round trip. This paragraph is here so that nobody has to
// wonder later why a domain check appeared and then vanished.
class ChannelInviteModel : public QObject {
    Q_OBJECT

    // A request is in flight. The dialog disables Add on this rather than
    // tracking anything finer: there is exactly one request in this surface.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Last failure, phrased as what to do about it rather than as what the
    // server said. Empty when the last attempt succeeded. Rendered inline
    // next to the field, not as a toast — the thing that failed is right
    // there and the message belongs beside it.
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    // Last success, in the words that match what actually happened — which
    // differs for a bot and for a person, and that difference is the single
    // most surprising thing about this feature for anyone arriving from
    // Discord. Empty until something succeeds.
    Q_PROPERTY(QString noticeText READ noticeText NOTIFY noticeTextChanged)

public:
    explicit ChannelInviteModel(QObject* parent = nullptr);

    // The seams. ServerConnection fills these in; the test records the calls.
    // Every one is optional: an unset hook makes the model behave as if the
    // answer were unknown rather than crashing, which is what a dialog opened
    // against a disconnected server should do.
    struct Hooks {
        // POST the invite. The reply comes back through onInvited/onFailed.
        std::function<void(const QString& roomId, const QString& userId)> invite;
        // The client's cached membership for (room, user): "join", "invite",
        // "leave", "ban", or empty when the client has never seen a member
        // event for them there. A cache, never an authority — the server
        // re-checks everything and its answer wins.
        //
        // Read by readmitHint() and nothing else: only the "leave" branch has
        // a consumer now. It is not used to decide whether to send a request,
        // and must not be — see invite().
        std::function<QString(const QString& roomId, const QString& userId)> membershipOf;
        // Whether the client already knows `userId` is a bot. True only when
        // it knows; "false" means "not known to be one", never "is a human".
        // See noticeFor() for why the copy is built to survive that.
        std::function<bool(const QString& userId)> isKnownBot;
        // There was a selfUserId hook here, read only by the homeserver
        // guess. Nothing in this model needs to know which account we are
        // any more, so it went with the guess rather than sitting wired up
        // and unread. The locked-state copy that names the signed-in account
        // is in AddMemberDialog.qml and reads ServerConnection::userId
        // directly; it never came through here.

        // Display name for `userId` if the client has one, else empty.
        std::function<QString(const QString& userId)> displayNameOf;
    };
    Hooks hooks;

    bool busy() const { return m_busy; }
    QString errorText() const { return m_errorText; }
    QString noticeText() const { return m_noticeText; }

    // ── actions (called from QML) ──────────────────────────────────────────

    // Send the invite for `userId` into `roomId`. The only thing refused
    // locally, without spending a request, is a structurally malformed id —
    // everything else is the server's answer to give. (An already-joined
    // target used to be refused here too; see the header and invite().)
    //
    // `roomName` is copy only and is currently unread: it was the channel
    // name in that refusal. Kept because the reply handlers onInvited/
    // onFailed take it for the same reason and the dialog passes it to all
    // three, so dropping it from one would make the surface asymmetric for
    // no gain.
    Q_INVOKABLE void invite(const QString& roomId, const QString& userId,
                            const QString& roomName = {});

    // Clear the error and notice lines. Called when the dialog opens and when
    // the field is edited, so a stale answer never sits under a new attempt.
    Q_INVOKABLE void reset();

    // ── field validation ───────────────────────────────────────────────────

    // Empty string when `userId` is a structurally usable mxid, otherwise the
    // reason, phrased with an example. Called on every keystroke so Add can be
    // disabled rather than attempted.
    //
    // STRUCTURE ONLY — `@`, one `:`, both halves non-empty, no whitespace. It
    // deliberately does not police the localpart grammar the way
    // BotAdminModel::localpartError does: that one is validating something the
    // user is CREATING, where the server's rule is knowable and a rejection is
    // cheap to avoid. This one is validating something that already exists,
    // where being stricter than the server means refusing to type a real
    // person's id, and the server's answer is one request away.
    static QString userIdError(const QString& userId);
    Q_INVOKABLE QString validateUserId(const QString& userId) const
    {
        return userIdError(userId);
    }

    // There is no homeserverWarning()/warnAboutHomeserver() pair here any
    // more. It warned when the typed id's domain was not ours, as the closest
    // the client could get to "no such user" while the server was still
    // accepting invites for accounts that did not exist. The server answers
    // that case itself now — see the note in the .cpp, above readmitHint, for
    // the full reasoning and for why it should not come back.

    // A note, shown while typing, for someone whose last membership in this
    // channel was a departure — "not an error, and here is what adding them
    // will do".
    //
    // It exists because of the one case on this path that is easiest to read
    // as a failure and is in fact the feature. /join refuses a user the
    // server's was_removed_by_moderator() recognises, with "you were removed
    // from this channel and cannot rejoin unless you are invited back", and
    // POST /invite is the ONLY thing that clears that — RoomHandler.cpp says
    // so where it refuses the join. So the person looking at this dialog is
    // frequently the moderator holding the one key, and nothing anywhere told
    // them the door was theirs to open.
    //
    // It does NOT distinguish a moderator's removal from a voluntary leave,
    // and deliberately so: the marker that separates them
    // (`bsfchat.removed_by` on the member event) is documented server-side as
    // server-local, with "no client has to understand it" — reading it here
    // would couple this dialog to an implementation detail the server is free
    // to drop, to save one word. Auto-join puts everyone in every channel, so
    // leaving is also just how somebody hides a channel; the copy covers both
    // truthfully instead.
    Q_INVOKABLE QString readmitHint(const QString& roomId, const QString& userId,
                                    const QString& roomName = {}) const;

    // The sentence the dialog shows before anything is typed, naming what
    // adding a BOT does differently. Lives here rather than in the QML so the
    // test can assert it actually says so — this is the one behaviour a
    // Discord user will not predict, and a string that only exists in a
    // binding is a string that can quietly stop saying it.
    Q_INVOKABLE QString botAdvisory() const;

    // ── replies (wired from ServerConnection) ──────────────────────────────

    // The invite was accepted. `userId`/`roomName` echo what was asked for.
    void onInvited(const QString& userId, const QString& roomName);
    // `status` is the HTTP status (0 when the request never reached the
    // server), `message` the server's own `error` text.
    void onFailed(const QString& userId, const QString& roomName,
                  int status, const QString& message);

    // Exposed for the test, and because the mapping is the substance of this
    // class. See the .cpp for why it matches on the server's message text.
    static QString explainFailure(const QString& userId, const QString& roomName,
                                  int status, const QString& message);

signals:
    void busyChanged();
    void errorTextChanged();
    void noticeTextChanged();
    // A bot was joined to a channel just now. ServerConnection listens so the
    // roster can catch up without waiting for the next sync tick.
    void memberAdded(const QString& roomId, const QString& userId);

private:
    void setBusy(bool busy);
    void setErrorText(const QString& text);
    void setNoticeText(const QString& text);
    // The success sentence. Branches on whether the client KNOWS the target is
    // a bot; see the .cpp for what it says when it does not.
    QString noticeFor(const QString& userId, const QString& roomName) const;
    QString nameFor(const QString& userId) const;

    bool m_busy = false;
    QString m_errorText;
    QString m_noticeText;
    // The request in flight, so a reply can be phrased about the right target
    // even if the field has since been retyped.
    QString m_pendingRoomId;
};
