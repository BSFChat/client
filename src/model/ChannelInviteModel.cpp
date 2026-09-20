#include "model/ChannelInviteModel.h"

#include <QRegularExpression>

namespace {

// "#general" when we have a name, "this channel" when we do not. Every message
// in this file reads better with the channel named and none of them may break
// when it is missing — a dialog opened from the member list always has one, a
// reply that lands after a channel switch may not.
QString channelPhrase(const QString& roomName)
{
    return roomName.isEmpty() ? QStringLiteral("this channel")
                              : QStringLiteral("#") + roomName;
}

} // namespace

ChannelInviteModel::ChannelInviteModel(QObject* parent)
    : QObject(parent)
{
}

void ChannelInviteModel::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void ChannelInviteModel::setErrorText(const QString& text)
{
    if (m_errorText == text) return;
    m_errorText = text;
    emit errorTextChanged();
}

void ChannelInviteModel::setNoticeText(const QString& text)
{
    if (m_noticeText == text) return;
    m_noticeText = text;
    emit noticeTextChanged();
}

QString ChannelInviteModel::userIdError(const QString& userId)
{
    if (userId.isEmpty()) return {};

    // Whitespace first: it is the one mistake that survives every other check
    // (a pasted id with a trailing newline is structurally perfect) and the
    // one whose server-side failure is least legible.
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    if (userId.contains(whitespace))
        return QStringLiteral("No spaces in a member id.");

    if (!userId.startsWith(QLatin1Char('@')))
        return QStringLiteral("A member id starts with @ — like @alice:bsfchat.com.");

    const int colon = userId.indexOf(':');
    if (colon < 0) {
        return QStringLiteral("Add the server after a colon — like "
                              "@alice:bsfchat.com.");
    }
    // Everything before the colon minus the leading '@'.
    if (colon <= 1)
        return QStringLiteral("There's no name before the colon.");
    if (colon == userId.size() - 1)
        return QStringLiteral("There's no server after the colon.");

    // A second colon makes the domain ambiguous. Not folded in with the
    // "no colon" case above because the advice differs: one is "you left
    // something out", this is "you have one too many".
    if (userId.indexOf(':', colon + 1) >= 0)
        return QStringLiteral("A member id has one colon, not two.");

    return {};
}

// THERE WAS A homeserverWarning() HERE, AND ITS ABSENCE IS DELIBERATE.
//
// It cautioned — in warn colour, under the field — when the typed id's
// homeserver was not the one we are signed in to, and it existed because of a
// server bug rather than because the domain mattered. Until server b8e26ac,
// handle_invite never called user_exists() and there is no foreign key on the
// membership table, so inviting an id nobody held returned 200 and wrote a
// membership row for nobody. The dialog announced a success, the person never
// turned up, and nothing in the product ever surfaced the row.
//
// The client could not detect that, so it guessed at the commonest shape of
// the mistake: this deployment does not federate, so every real member id
// ends in our homeserver, so a different domain is probably a typo. A guess,
// with the two failings a guess has — it was blind to a typo'd LOCALPART,
// which is the more common slip, and it would have started crying wolf on
// every genuinely remote id the day federation arrived.
//
// The server now refuses the whole class with one 403 (kNoSuchAccount in
// RoomHandler.cpp — a typo, a remote id and a malformed string all get it),
// so the guess is dead code and its job has moved into explainFailure below.
// userIdError() above stays: that is structure, checkable without asking, and
// it saves a round trip rather than predicting an answer.
//
// If federation ever lands, do not restore this. "Not a local account" stops
// being the same statement as "does not exist" on the SERVER first, and the
// server's wording is what changes.

QString ChannelInviteModel::readmitHint(const QString& roomId, const QString& userId,
                                        const QString& roomName) const
{
    if (roomId.isEmpty() || !userIdError(userId).isEmpty()) return {};
    if (!hooks.membershipOf) return {};
    // Only "leave". "ban" is the server's answer to give, with the remedy that
    // goes with it (see explainFailure) — offering to add someone who is
    // banned would be offering something that cannot work.
    if (hooks.membershipOf(roomId, userId) != QLatin1String("leave")) return {};

    return nameFor(userId) + QStringLiteral(" left or was removed from ")
         + channelPhrase(roomName)
         + QStringLiteral(". Adding them lets them back in — for someone a "
                          "moderator removed, this is the only thing that does.");
}

QString ChannelInviteModel::botAdvisory() const
{
    // Said up front, every time, whether or not the id typed so far looks
    // like a bot — because the client frequently cannot tell (see noticeFor)
    // and because the point of the sentence is to set the expectation BEFORE
    // the click, not to explain the result afterwards.
    return QStringLiteral(
        "A person gets an invite and joins when they accept. A bot joins "
        "straight away — there's no pending invite for it to accept, and "
        "everyone in the channel sees it arrive.");
}

QString ChannelInviteModel::nameFor(const QString& userId) const
{
    if (hooks.displayNameOf) {
        const QString dn = hooks.displayNameOf(userId);
        if (!dn.isEmpty()) return dn;
    }
    return userId;
}

QString ChannelInviteModel::noticeFor(const QString& userId,
                                      const QString& roomName) const
{
    const QString who  = nameFor(userId);
    const QString were = channelPhrase(roomName);

    // The client knows a user is a bot from `bsfchat.bot` riding in the
    // m.room.member events it has already seen, plus anything the bot admin
    // pane has listed. That covers the common cases — a bot just created, or
    // one already in another channel — and it covers NOTHING for a bot on a
    // server whose bot list the signed-in account may not read (that list is
    // gated on MANAGE_BOTS; this dialog is gated on MANAGE_CHANNELS, and
    // neither implies the other).
    //
    // When we know it is a bot, we say what happened, because "it is already
    // here, there is nothing to accept" is the surprising part for anyone
    // arriving from Discord and it is true of every bot outcome.
    //
    // When we do NOT know, we say only that they were added, and nothing
    // about what happens next. Three outcomes reach this line and the server
    // answers `{}` for all of them and cannot be asked which it did: a person
    // who now has an invite to accept, a bot that joined on the spot, and —
    // since the server stopped demoting a joined member — someone who was
    // already in the channel and for whom nothing at all was written. The old
    // copy named the first two ("a person sees the invite next time they
    // connect; a bot is already in the channel") and was a flat lie for the
    // third. A sentence that covers all three says nothing useful, so this
    // says the one thing that is true in every case and stops.
    if (hooks.isKnownBot && hooks.isKnownBot(userId)) {
        return who + QStringLiteral(" is a bot, so it has joined ") + were
             + QStringLiteral(" already — no invite to accept.");
    }
    return QStringLiteral("Added ") + who + QStringLiteral(" to ") + were
         + QStringLiteral(".");
}

void ChannelInviteModel::reset()
{
    setErrorText({});
    setNoticeText({});
}

void ChannelInviteModel::invite(const QString& roomId, const QString& userId,
                                const QString& roomName)
{
    Q_UNUSED(roomName)   // see the declaration: copy for a refusal that went
    setNoticeText({});

    if (roomId.isEmpty()) {
        setErrorText(QStringLiteral("No channel is selected."));
        return;
    }

    const QString trimmed = userId.trimmed();
    const QString problem = userIdError(trimmed);
    if (!problem.isEmpty()) {
        setErrorText(problem);
        return;
    }
    if (trimmed.isEmpty()) {
        setErrorText(QStringLiteral("Type the member id to add — like "
                                    "@alice:bsfchat.com."));
        return;
    }

    // There was a local "already here" refusal here, reading the cached
    // roster, because handle_invite used to write membership 'invite' over an
    // existing 'join' — demoting a member who was already in the channel and
    // taking them out of everyone else's roster until they joined again.
    // Server a19fd10 (fix/invite-no-demote) makes that case a writeless 200,
    // the same answer the bot path always gave, so there is nothing left to
    // protect against and the request always goes to the server now.
    //
    // Do not reinstate it. It was never reliable: the roster is a cache, a
    // miss fell straight through to the bug, so all it ever decided was
    // whether the dialog said "already in" or announced a success — the same
    // click, two different answers, depending on what the client happened to
    // have seen. noticeFor()'s success copy is outcome-neutral for exactly
    // this reason, so a no-op reads true.
    if (!hooks.invite) {
        setErrorText(QStringLiteral("Not connected to this server."));
        return;
    }

    m_pendingRoomId = roomId;
    setErrorText({});
    setBusy(true);
    hooks.invite(roomId, trimmed);
}

QString ChannelInviteModel::explainFailure(const QString& userId,
                                           const QString& roomName,
                                           int status, const QString& message)
{
    const QString who   = userId.isEmpty() ? QStringLiteral("that member") : userId;
    const QString where = channelPhrase(roomName);

    // MATCHED ON THE SERVER'S TEXT, and that needs defending.
    //
    // handle_invite answers seven genuinely different situations with the same
    // 403 and the same errcode M_FORBIDDEN — caller not in the room, room is a
    // DM, caller lacks MANAGE_CHANNELS, target banned here, target banned
    // server-wide, target has no account at all, target is a deactivated bot.
    // The `error` string is the only thing that tells them apart, so it is
    // what we match on. A distinct errcode per case would be better and
    // belongs on the server side; until it exists, this is the whole of the
    // available signal.
    //
    // Every branch is therefore a SUBSTRING match on the distinctive words
    // rather than on the whole sentence, and NO TWO OF THOSE FRAGMENTS MAY
    // MATCH EACH OTHER'S SENTENCE — a collision would answer one refusal with
    // another's advice, confidently and wrongly. That is checked behaviourally
    // by noRefusalIsCapturedByAnotherRefusalsSubstring, which feeds the
    // server's seven strings in and requires seven distinct answers; add an
    // eighth branch and add its string there. The default hands back the
    // server's own text. Reword a message upstream and this degrades to
    // showing what the server said — worse copy, never a wrong claim and never
    // a swallowed error. tests/test_channel_invite.cpp pins the strings
    // against the ones in RoomHandler.cpp so the drift is at least visible.
    const QString m = message.toLower();
    const auto says = [&m](const char* fragment) {
        return m.contains(QLatin1String(fragment));
    };

    if (status == 403) {
        if (says("insufficient permissions")) {
            return QStringLiteral("You need the Manage Channels permission in ")
                 + where + QStringLiteral(" to add members. Ask a server admin "
                                          "to give your role that permission.");
        }
        if (says("not a member of this room")) {
            return QStringLiteral("You're not in ") + where
                 + QStringLiteral(" yourself, so you can't add anyone to it.");
        }
        if (says("direct message")) {
            return QStringLiteral("A direct message is just the two of you and "
                                  "can't take a third person. Add them to a "
                                  "channel instead.");
        }
        if (says("banned from this server")) {
            return who + QStringLiteral(" is banned from this server. Lift the "
                                        "ban under Server Settings → Bans "
                                        "first — it covers every channel.");
        }
        if (says("banned from this room")) {
            return who + QStringLiteral(" is banned from ") + where
                 + QStringLiteral(". Lift the ban under Server Settings → "
                                  "Bans before adding them back.");
        }
        // No such account. Placed here, after both bans and before the bot
        // branch, because that is where the server evaluates it — a pre-ban
        // on an id nobody has registered yet still answers "banned from this
        // server", which is both true and the more actionable of the two.
        //
        // The message does not name the channel. This refusal is about the id
        // alone and would be identical in every channel on the server, so
        // naming one would imply a different channel might have worked — the
        // same reasoning the 400 branch is built on.
        //
        // One sentence covers a typo'd localpart, a typo'd domain and an id
        // from another server, because the server answers all three with this
        // and cannot be asked which it meant. It is also the sentence that
        // replaced homeserverWarning() — see the note above readmitHint for
        // why the client used to guess at the third case and no longer does.
        //
        // Built around `userId` rather than `who`, because `who`'s "that
        // member" fallback would read "There's no account that member on this
        // server." Every other branch puts `who` in a slot where the fallback
        // still parses; this one does not, so it says "with that id" instead.
        if (says("no account")) {
            const QString lead =
                userId.isEmpty()
                    ? QStringLiteral("There's no account with that id on this server.")
                    : QStringLiteral("There's no account ") + userId
                          + QStringLiteral(" on this server.");
            return lead + QStringLiteral(" Check the spelling — ids from "
                                         "another server can't be added here.");
        }
        if (says("deactivated")) {
            return who + QStringLiteral(" is a deactivated bot and can't be "
                                        "added to a channel. Deactivation is "
                                        "permanent — create a new bot instead.");
        }
    }

    if (status == 400) {
        return QStringLiteral("The server didn't accept \"") + who
             + QStringLiteral("\" as a member id. It should look like "
                              "@alice:bsfchat.com.");
    }
    if (status == 404) {
        return where + QStringLiteral(" no longer exists on this server.");
    }
    // 0 is "the request never got an HTTP answer" — DNS, TLS, a dropped
    // connection. Nothing about the invite is wrong, so the message must not
    // suggest that anything about it is.
    if (status == 0) {
        return QStringLiteral("Couldn't reach the server. Check your connection "
                              "and try again — nothing was added.");
    }

    if (message.isEmpty())
        return QStringLiteral("Couldn't add ") + who + QStringLiteral(" to ") + where
             + QStringLiteral(".");
    return QStringLiteral("Couldn't add ") + who + QStringLiteral(" to ") + where
         + QStringLiteral(": ") + message;
}

void ChannelInviteModel::onInvited(const QString& userId, const QString& roomName)
{
    setBusy(false);
    setErrorText({});
    setNoticeText(noticeFor(userId, roomName));
    if (!m_pendingRoomId.isEmpty()) emit memberAdded(m_pendingRoomId, userId);
    m_pendingRoomId.clear();
}

void ChannelInviteModel::onFailed(const QString& userId, const QString& roomName,
                                  int status, const QString& message)
{
    setBusy(false);
    setNoticeText({});
    setErrorText(explainFailure(userId, roomName, status, message));
    m_pendingRoomId.clear();
}
