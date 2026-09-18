#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>
#include <string>

#include <bsfchat/Constants.h>
#include <bsfchat/MatrixTypes.h>

// One entry of a /members reply → the RoomEvent the member models read.
//
// Every other path that reaches MemberListModel::processEvent hands it an
// event the protocol parsed, whose `content` is whatever the server sent,
// copied across whole. The /members reply is the one exception: it arrives as
// Qt JSON (MatrixClient::roomMembersResult emits a QJsonArray), so the event
// has to be REBUILT here, field by field — and a field nobody remembers to
// name is silently dropped.
//
// That is not a hypothetical. `bsfchat.nickname` was missing from this
// rebuild from the initial commit, and `avatar_url` with it, while
// processEvent has always read both. The cost is paid on every channel open,
// because the handler clears the model first: the FIRST roster a user sees is
// the one built here, and a field missing from it stays missing until the
// next m.room.member for that user happens to arrive over /sync, which may be
// a very long time.
//
// So the rule for this function is: it must name every content key
// MemberListModel::processEvent reads, and it is the only place that has to.
// The companion test walks the model's readers against this list, so adding a
// field to processEvent without adding it here fails the build's tests rather
// than quietly blanking a column.
//
// Extracted to a header, like ServerRoster and MemberCache, so it can be
// driven without a ServerConnection, an event loop or a network.
namespace bsfchat::client {

// Returns the member event for `obj`, or nullopt when `obj` is not an
// m.room.member whose membership is "join" — the only rows this roster
// path builds. (A leave/ban row would only be removed again immediately;
// the model is cleared before the loop runs.)
inline std::optional<bsfchat::RoomEvent> joinedMemberEventFromJson(const QJsonObject& obj)
{
    if (obj.value(QStringLiteral("type")).toString()
        != QString::fromUtf8(bsfchat::event_type::kRoomMember)) {
        return std::nullopt;
    }

    const QJsonObject content = obj.value(QStringLiteral("content")).toObject();
    if (content.value(QStringLiteral("membership")).toString() != QStringLiteral("join")) {
        return std::nullopt;
    }

    const QString userId = obj.value(QStringLiteral("state_key")).toString();

    bsfchat::RoomEvent ev;
    ev.type = std::string(bsfchat::event_type::kRoomMember);
    ev.sender = userId.toStdString();
    ev.state_key = userId.toStdString();
    ev.content.data = {{"membership", "join"}};

    // An empty string is how the server spells "not set" for all three of
    // these, and processEvent's own default is the empty string, so leaving
    // the key out is the same value with less to carry.
    const auto copyString = [&](const char* key) {
        const QString value = content.value(QString::fromUtf8(key)).toString();
        if (!value.isEmpty()) ev.content.data[key] = value.toStdString();
    };
    // The EFFECTIVE name — the server resolves nickname-or-global before it
    // writes this, so it is already the string to render.
    copyString("displayname");
    // The per-server nickname, which DisplayNameRole cannot be reverse
    // engineered from: it is what lets admin UI tell "has a nickname" apart
    // from "global name happens to look like this". See the comment on
    // MemberListModel::NicknameRole.
    copyString("bsfchat.nickname");
    copyString("avatar_url");

    // Absent means false — the server never writes `false` — so an absent key
    // is copied as an absent key, not as an explicit false.
    if (content.value(QStringLiteral("bsfchat.bot")).toBool(false)) {
        ev.content.data["bsfchat.bot"] = true;
    }

    return ev;
}

// True when `ev` (from joinedMemberEventFromJson) is a bot account, for the
// caller's own bot-flag cache.
inline bool memberEventIsBot(const bsfchat::RoomEvent& ev)
{
    return ev.content.data.value(std::string("bsfchat.bot"), false);
}

}  // namespace bsfchat::client
