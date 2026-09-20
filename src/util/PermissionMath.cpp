#include "util/PermissionMath.h"

#include <algorithm>

namespace bsfchat::permmath {

namespace {

// Is this user id a bot's, judged from the id alone?
//
// This mirror's copy of protocol's `bot::is_bot_user_id`
// (protocol/include/bsfchat/Constants.h), kept deliberately byte-for-byte
// equivalent to it: a leading '@', then kBotLocalpartPrefix. Qt's startsWith
// covers the length check protocol spells out, and is case-sensitive, which is
// the comparison protocol makes.
//
// Mirrored rather than included for the reason the header gives — this file
// links without the protocol library so the maths stay unit-testable — and
// tests/test_models.cpp pins the two against each other over a table of ids so
// the copy cannot drift into a wider or narrower rule than the server's.
bool isBotUserId(const QString& userId)
{
    return userId.startsWith(QLatin1Char('@'))
        && QStringView(userId).sliced(1).startsWith(
               QLatin1String(kBotLocalpartPrefix));
}

} // namespace

bool inheritsEveryoneRole(const QString& userId)
{
    return !isBotUserId(userId);
}

Flags effectivePermissions(const QVector<Role>& allRoles,
                           const QStringList& myRoleIds,
                           const QString& userId,
                           const QVector<Override>* channelOverrides)
{
    const QString everyoneId = QString::fromLatin1(kEveryoneRoleId);

    // Asked ONCE and used in both branches below, exactly as
    // PermissionsEngine::compute asks it once.
    const bool implicitEveryone = inheritsEveryoneRole(userId);

    // Nothing synced yet. The server answers this case with the @everyone
    // defaults rather than "no permissions" (see PermissionsEngine::compute), so
    // mirroring it here is what stops a client that hasn't received the roles
    // state event from greying out the composer and telling the user they lack
    // permission to speak — a message the server would have accepted.
    //
    // For an account the default does not apply to, the answer is nothing. That
    // is not an exception to the rule but its most dangerous corner: this branch
    // hands out kEveryoneDefault with no document to read it out of, which is
    // precisely the grant a scoped bot was not given.
    if (allRoles.isEmpty() && myRoleIds.isEmpty()) {
        return implicitEveryone ? kEveryoneDefault : Flags(0);
    }

    auto findRole = [&](const QString& id) -> const Role* {
        for (const auto& r : allRoles) {
            if (r.id == id) return &r;
        }
        return nullptr;
    };

    // @everyone is implicit — applied whether or not it is listed in myRoleIds —
    // for every account the default role applies to. For one it does not, it is
    // an ORDINARY role: not handed over, but still assignable by id like any
    // other, so "let this bot do what a member can" remains expressible and
    // simply has to be said on purpose.
    QVector<Role> mine;
    mine.reserve(myRoleIds.size() + 1);
    if (implicitEveryone) {
        if (const Role* e = findRole(everyoneId)) mine.append(*e);
    }
    for (const auto& id : myRoleIds) {
        // Skipped only because it is already there. When it is not implicit, an
        // assignment naming @everyone resolves it like any other role.
        if (implicitEveryone && id == everyoneId) continue;
        if (const Role* r = findRole(id)) mine.append(*r);
    }
    std::stable_sort(mine.begin(), mine.end(),
                     [](const Role& a, const Role& b) { return a.position < b.position; });

    Flags base = 0;
    for (const auto& r : mine) base |= r.permissions;

    if ((base & kAdministrator) != 0) return kAllFlags;

    // Server scope: channel overrides are not part of the answer.
    if (channelOverrides == nullptr) return base;

    auto apply = [&](const QString& key) {
        for (const auto& ov : *channelOverrides) {
            if (ov.targetKey == key) {
                base = (base & ~ov.deny) | ov.allow;
                return;
            }
        }
    };

    // Unconditional, including for an account that did not inherit the ROLE. An
    // override is a statement about a CHANNEL ("this one is open to everybody")
    // rather than about who holds which role, so withholding the implicit role
    // does not withhold the channel's @everyone override —
    // permission::inherits_everyone_role calls this out by name, and compute()
    // applies it the same way.
    apply(QStringLiteral("role:") + everyoneId);
    for (const auto& r : mine) {
        if (r.id == everyoneId) continue;
        apply(QStringLiteral("role:") + r.id);
    }
    apply(QStringLiteral("user:") + userId);

    return base;
}

} // namespace bsfchat::permmath
