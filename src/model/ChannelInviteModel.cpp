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

// The homeserver half of an mxid, or empty if it does not have one.
QString domainOf(const QString& userId)
{
    const int colon = userId.indexOf(':');
    if (colon < 0) return {};
    return userId.mid(colon + 1);
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

QString ChannelInviteModel::homeserverWarning(const QString& userId,
                                              const QString& selfUserId)
{
    if (!userIdError(userId).isEmpty()) return {};
    const QString theirs = domainOf(userId);
    const QString ours   = domainOf(selfUserId);
    if (theirs.isEmpty() || ours.isEmpty()) return {};
    // Case-insensitively: a homeserver name is a hostname, and "BSFChat.com"
    // and "bsfchat.com" are the same server. Warning about that difference
    // would be warning about nothing.
    if (theirs.compare(ours, Qt::CaseInsensitive) == 0) return {};

    return QStringLiteral("That id is on \"") + theirs
         + QStringLiteral("\", not \"") + ours
         + QStringLiteral("\". Members of this server end in \"") + ours
         + QStringLiteral("\" — check for a typo. The server accepts this "
                          "either way and the member simply never appears.");
}

QString ChannelInviteModel::warnAboutHomeserver(const QString& userId) const
{
    return homeserverWarning(userId, hooks.selfUserId ? hooks.selfUserId() : QString());
}

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
    // So there are two sentences, not three. When we know, we say what
    // happened. When we do not, we say what happened for BOTH kinds rather
    // than asserting the human case and being wrong in front of someone
    // watching a bot appear instantly. The server returns `{}` for both and
    // cannot be asked which it did.
    if (hooks.isKnownBot && hooks.isKnownBot(userId)) {
        return who + QStringLiteral(" is a bot, so it has joined ") + were
             + QStringLiteral(" already — no invite to accept.");
    }
    return QStringLiteral("Added ") + who + QStringLiteral(" to ") + were
         + QStringLiteral(". A person sees the invite next time they connect; "
                          "a bot is already in the channel.");
}

void ChannelInviteModel::reset()
{
    setErrorText({});
    setNoticeText({});
}

void ChannelInviteModel::invite(const QString& roomId, const QString& userId,
                                const QString& roomName)
{
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

    // "Already here" is refused HERE and nowhere else, because the server does
    // not refuse it. For a bot it is a documented writeless 200, which would
    // leave the dialog announcing a join that did not happen. For a human it
    // is worse than a no-op: handle_invite writes membership 'invite' over the
    // existing 'join', which demotes a member who was already in the channel
    // and, until they join again, takes them out of the roster everyone else
    // sees. Nobody asking to add a member is asking for that.
    //
    // The roster is a cache and it can be stale, so this only ever fires on a
    // membership the client has actually seen. An unknown user falls through
    // to the server, which is the right way round: refusing on a cache miss
    // would make the dialog unusable on a fresh connection.
    if (hooks.membershipOf) {
        const QString membership = hooks.membershipOf(roomId, trimmed);
        if (membership == QLatin1String("join")) {
            setErrorText(nameFor(trimmed) + QStringLiteral(" is already in ")
                         + channelPhrase(roomName) + QStringLiteral("."));
            return;
        }
    }

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
    // handle_invite answers six genuinely different situations with the same
    // 403 and the same errcode M_FORBIDDEN — caller not in the room, room is a
    // DM, caller lacks MANAGE_CHANNELS, target banned here, target banned
    // server-wide, target is a deactivated bot. The `error` string is the only
    // thing that tells them apart, so it is what we match on. A distinct
    // errcode per case would be better and belongs on the server side; until
    // it exists, this is the whole of the available signal.
    //
    // Every branch is therefore a SUBSTRING match on the distinctive words
    // rather than on the whole sentence, and the default hands back the
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
