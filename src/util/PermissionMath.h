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
//
// That mirroring now covers one RULE as well as the bit values: whether the
// implicit @everyone role applies to an account at all. See
// inheritsEveryoneRole below.
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

// The flags that open at least one PAGE in the Server Settings modal, and so
// the set that decides whether the gear in the channel-list header is a live
// control or a locked one.
//
// It exists because the gear's gate and the modal's nav list are one fact
// expressed twice, and they had drifted: the gear asked for MANAGE_ROLES,
// MANAGE_CHANNELS, KICK or BAN, while the modal also carries an Overview page
// gated on MANAGE_SERVER and a Bots page gated on MANAGE_BOTS. A member holding
// only one of those two had no way in at all — including, absurdly, to the
// "you need the Manage bots permission" state BotManagerPane renders for
// exactly that member.
//
// Keep this in step with the nav Repeater's model in
// qml/components/ServerSettings.qml. tests/test_bots.cpp checks that every
// entry here is a flag protocol knows about, and that the two lists are the
// same length.
constexpr Flags kServerSettingsPages =
    kManageServer | kManageRoles | kManageChannels | kKickMembers |
    kBanMembers | kManageBots | kAdministrator;

// Whether `mask` (an already-resolved SERVER-SCOPE permission set) opens the
// Server Settings modal onto anything at all. Server scope is not a detail:
// every page in that modal is server-wide, and asking with a channel's
// overrides folded in would open a dialog whose every write the server refuses.
constexpr bool opensServerSettings(Flags mask)
{
    return (mask & kServerSettingsPages) != 0;
}

constexpr Flags kAllFlags =
    kViewChannel | kSendMessages | kAttachFiles | kEmbedLinks | kManageMessages |
    kManageChannels | kManageRoles | kKickMembers | kBanMembers |
    kMentionEveryone | kManageServer | kChangeNickname | kManageNicknames |
    kManageBots | kAddReactions | kAdministrator;

inline const char* kEveryoneRoleId = "everyone";

// The reserved localpart prefix that marks a bot account.
//
// This mirror's own copy of protocol's `bot::kLocalpartPrefix`
// (protocol/include/bsfchat/Constants.h), carried here for the same reason as
// every bit value above: so the permission maths link without the protocol
// library. tests/test_models.cpp pins it against protocol's, exactly as
// tests/test_bots.cpp pins kManageBots, so a change to the namespace cannot
// land quietly on this side.
inline const char* kBotLocalpartPrefix = "bot_";

// Does the implicit @everyone role apply to this account? False for a bot.
//
// The authority is `permission::inherits_everyone_role`
// (protocol/include/bsfchat/Permissions.h); read it for the whole argument.
// The short version: @everyone is the default role FOR PEOPLE WHO JOIN the
// server, and a bot is not somebody who joined — it is an account an
// administrator minted for one job, already excluded from the other two things
// joining confers (it cannot log in with a password, and it is excluded from
// channel auto-join). This is the third exclusion and the one with
// consequences, because a role only ever ADDS bits: while a bot held the
// default, "this bot may see these three channels and nothing else" was not a
// sentence the permission model could say.
//
// Withholding the implicit grant makes @everyone ORDINARY for a bot — still a
// role, still assignable by id — so "let this bot do what a member can" stays
// available and merely has to be said on purpose.
//
// Judged from the user id alone, which is sound because the `bot_` namespace is
// closed on every account-creation path: registration refuses the prefix, the
// OIDC auto-create path mints `oidc_` localparts, and bot creation refuses a
// localpart outside it. Takes a full user id ("@bot_deploy:example.com"), not a
// localpart.
bool inheritsEveryoneRole(const QString& userId);

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
//
// `userId` does two jobs, and the second is newer than most call sites: besides
// keying the `user:<id>` channel override, it decides whether the implicit
// @everyone role applies at all (inheritsEveryoneRole — it does not, for a
// bot). So a placeholder id is no longer safe to pass when the caller only
// cares about roles: an id that is not the account being evaluated can now
// change the BASE permissions and not just the overrides.
Flags effectivePermissions(const QVector<Role>& allRoles,
                           const QStringList& myRoleIds,
                           const QString& userId,
                           const QVector<Override>* channelOverrides);

} // namespace bsfchat::permmath
