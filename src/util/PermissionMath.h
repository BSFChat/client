#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

// Client-side mirror of the server's permission evaluation.
//
// This lives apart from ServerConnection for one reason: it decides what the UI
// lets you attempt, and when it disagrees with the server you either get a
// button that always 403s or — the failure the owner actually cares about — no
// button at all for something your role is entitled to do. That is worth having
// tests on, and ServerConnection cannot be instantiated in a unit test.
//
// Authority is server/src/auth/Permissions.cpp. Keep the two in step.
namespace bsfchat::permmath {

using Flags = std::uint64_t;

// Bit values from protocol/include/bsfchat/Permissions.h.
constexpr Flags kViewChannel     = 1ULL << 0;
constexpr Flags kSendMessages    = 1ULL << 1;
constexpr Flags kAttachFiles     = 1ULL << 2;
constexpr Flags kEmbedLinks      = 1ULL << 3;
constexpr Flags kManageMessages  = 1ULL << 4;
constexpr Flags kManageChannels  = 1ULL << 5;
constexpr Flags kManageRoles     = 1ULL << 6;
constexpr Flags kKickMembers     = 1ULL << 7;
constexpr Flags kBanMembers      = 1ULL << 8;
constexpr Flags kMentionEveryone = 1ULL << 9;
constexpr Flags kManageServer    = 1ULL << 10;
constexpr Flags kChangeNickname  = 1ULL << 11;
constexpr Flags kManageNicknames = 1ULL << 12;
// Create bot accounts, rotate their tokens and deactivate them. Evaluated at
// SERVER scope only — a bot belongs to the server, not to a channel, so a
// per-channel override must never light up the management dialog (see
// effectivePermissions below for why that distinction is load-bearing).
//
// This mirror is the client's own copy of the bit, exactly as every flag
// above it is. It is NOT waiting on protocol's Permissions.h to gain
// kManageBots: this file has always carried the values rather than including
// that header, so that the permission maths can be unit-tested without the
// protocol library, and the comment at the top of the namespace is the
// contract that keeps the two in step. If protocol ever assigns bit 13 to
// something else, the server and this client disagree about every role's
// permissions and the mismatch is not subtle — tests/test_bots.cpp pins the
// value so the change cannot land quietly on this side.
constexpr Flags kManageBots      = 1ULL << 13;
// Gate on reacting, separate from kSendMessages. Before it existed the server
// applied no permission check to m.reaction at all, so a member with
// SEND_MESSAGES denied could still react — muting somebody was not actually
// possible. Folding reactions into SEND_MESSAGES would have closed that while
// conflating two things moderators treat differently: a read-mostly channel
// where everyone may react but few may post is an ordinary arrangement.
//
// In kEveryoneDefault, deliberately and in step with protocol: the point is
// that denying reactions becomes possible, not that reacting becomes a
// privilege. A mirror that left it out here would make this client compute a
// smaller @everyone than the server grants and hide the reaction affordance
// from every member of a fresh server.
constexpr Flags kAddReactions    = 1ULL << 14;
constexpr Flags kAdministrator   = 1ULL << 15;

constexpr Flags kEveryoneDefault =
    kViewChannel | kSendMessages | kAttachFiles | kEmbedLinks | kChangeNickname |
    kAddReactions;

constexpr Flags kAllFlags =
    kViewChannel | kSendMessages | kAttachFiles | kEmbedLinks | kManageMessages |
    kManageChannels | kManageRoles | kKickMembers | kBanMembers |
    kMentionEveryone | kManageServer | kChangeNickname | kManageNicknames |
    kManageBots | kAddReactions | kAdministrator;

inline const char* kEveryoneRoleId = "everyone";

struct Role {
    QString id;
    int position = 0;
    Flags permissions = 0;
};

struct Override {
    QString targetKey; // "role:<id>" or "user:<mxid>"
    Flags allow = 0;
    Flags deny = 0;
};

// `channelOverrides` is null for a SERVER-SCOPE question ("may this user create
// a channel?") and non-null for a channel-scope one ("may they post here?").
//
// The distinction is load-bearing, not a convenience: a per-channel override
// must never answer a server-scope question. Granting someone MANAGE_CHANNELS
// in one channel lets them rename that channel; it must not make the client
// offer them a "create channel" affordance the server will refuse, and on the
// server side that same conflation was a privilege-escalation hole.
Flags effectivePermissions(const QVector<Role>& allRoles,
                           const QStringList& myRoleIds,
                           const QString& userId,
                           const QVector<Override>* channelOverrides);

} // namespace bsfchat::permmath
