#include "util/PermissionMath.h"

#include <algorithm>

namespace bsfchat::permmath {

Flags effectivePermissions(const QVector<Role>& allRoles,
                           const QStringList& myRoleIds,
                           const QString& userId,
                           const QVector<Override>* channelOverrides)
{
    const QString everyoneId = QString::fromLatin1(kEveryoneRoleId);

    // Nothing synced yet. The server answers this case with the @everyone
    // defaults rather than "no permissions" (see PermissionsEngine::compute), so
    // mirroring it here is what stops a client that hasn't received the roles
    // state event from greying out the composer and telling the user they lack
    // permission to speak — a message the server would have accepted.
    if (allRoles.isEmpty() && myRoleIds.isEmpty()) return kEveryoneDefault;

    auto findRole = [&](const QString& id) -> const Role* {
        for (const auto& r : allRoles) {
            if (r.id == id) return &r;
        }
        return nullptr;
    };

    // @everyone is implicit — applied whether or not it is listed in myRoleIds.
    QVector<Role> mine;
    mine.reserve(myRoleIds.size() + 1);
    if (const Role* e = findRole(everyoneId)) mine.append(*e);
    for (const auto& id : myRoleIds) {
        if (id == everyoneId) continue;
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

    apply(QStringLiteral("role:") + everyoneId);
    for (const auto& r : mine) {
        if (r.id == everyoneId) continue;
        apply(QStringLiteral("role:") + r.id);
    }
    apply(QStringLiteral("user:") + userId);

    return base;
}

} // namespace bsfchat::permmath
