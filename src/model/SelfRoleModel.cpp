#include "model/SelfRoleModel.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QVariantMap>

namespace {

// A role's permission bitfield arrives as a hex string ("0x1f") from the
// server and, on our own optimistic writes, as a number. Both shapes reach
// this model through the same QJsonArray, so normalise rather than trusting
// whichever one happened to land first.
std::uint64_t permsToNumber(const QJsonValue& v)
{
    if (v.isDouble()) return static_cast<std::uint64_t>(v.toDouble());
    if (!v.isString()) return 0;
    QString s = v.toString();
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) s = s.mid(2);
    bool ok = false;
    const auto parsed = s.toULongLong(&ok, 16);
    return ok ? parsed : 0;
}

constexpr QLatin1StringView kEveryone{"everyone"};

} // namespace

SelfRoleModel::SelfRoleModel(QObject* parent) : QObject(parent) {}

void SelfRoleModel::setServerState(const QJsonArray& serverRoles,
                                    const QStringList& heldRoleIds)
{
    m_all.clear();
    m_all.reserve(serverRoles.size());
    m_everyone = 0;

    for (const auto& v : serverRoles) {
        const auto o = v.toObject();
        Row r;
        r.id = o.value(QStringLiteral("id")).toString();
        // Same fallback ServerConnection::applyServerRolesEvent uses: legacy
        // documents key roles by name and carry no id at all.
        if (r.id.isEmpty()) r.id = o.value(QStringLiteral("name")).toString();
        if (r.id.isEmpty()) continue;
        r.name = o.value(QStringLiteral("name")).toString();
        if (r.name.isEmpty()) r.name = r.id;
        r.color = o.value(QStringLiteral("color")).toString();
        r.permissions = permsToNumber(o.value(QStringLiteral("permissions")));
        r.selfAssignable = o.value(QStringLiteral("self_assignable")).toBool(false);
        if (r.id == kEveryone) m_everyone = r.permissions;
        m_all.append(r);
    }

    m_loaded = !serverRoles.isEmpty();

    m_held.clear();
    for (const QString& id : heldRoleIds) m_held.insert(id);

    rebuild();
}

bool SelfRoleModel::effectiveHeld(const QString& roleId) const
{
    const auto it = m_pending.constFind(roleId);
    if (it != m_pending.constEnd()) return it.value();
    return m_held.contains(roleId);
}

void SelfRoleModel::rebuild()
{
    QVariantList out;
    for (const Row& r : m_all) {
        // @everyone is never opt-in: every member already has it and the
        // server refuses to mark it self-assignable at all. Belt and braces,
        // because a legacy document could still carry the flag.
        if (!r.selfAssignable || r.id == kEveryone) continue;

        const std::uint64_t extra = r.permissions & ~m_everyone;
        const bool blocked = extra != 0;
        const bool held = effectiveHeld(r.id);

        // See the header: an unheld blocked role is not on offer, so it is not
        // shown. A held one is, because the member is wearing it and cannot
        // take it off.
        if (blocked && !held) continue;

        QVariantMap m;
        m[QStringLiteral("id")] = r.id;
        m[QStringLiteral("name")] = r.name;
        m[QStringLiteral("color")] = r.color;
        m[QStringLiteral("held")] = held;
        m[QStringLiteral("pending")] = m_pending.contains(r.id);
        m[QStringLiteral("blocked")] = blocked;
        m[QStringLiteral("blockedReason")] =
            blocked ? tr("This role now grants more than @everyone does, so the "
                         "server will not let it be added or removed until an "
                         "administrator fixes it.")
                    : QString();
        out.append(m);
    }

    m_view = out;
    emit rolesChanged();
}

void SelfRoleModel::toggle(const QString& roleId)
{
    if (roleId.isEmpty() || roleId == kEveryone) return;
    // One request per role at a time. A second click while the first is in
    // flight would race two writes to the same list and leave whichever reply
    // landed last as the truth, which is not what the second click meant.
    if (m_pending.contains(roleId)) return;

    const Row* row = nullptr;
    for (const Row& r : m_all) {
        if (r.id == roleId) { row = &r; break; }
    }
    if (!row || !row->selfAssignable) return;
    // Refusing here is not security — the server re-checks — it is declining
    // to send a request we already know the answer to.
    if ((row->permissions & ~m_everyone) != 0) return;

    const bool want = !effectiveHeld(roleId);
    auto& hook = want ? hooks.addSelfRole : hooks.removeSelfRole;
    if (!hook) return;

    m_pending.insert(roleId, want);
    rebuild();
    hook(roleId);
}

void SelfRoleModel::onSelfRoleResult(const QString& roleId, const QStringList& roleIds)
{
    m_pending.remove(roleId);
    // The reply carries the whole post-change list, so adopt it wholesale
    // rather than applying the change we asked for. These differ whenever the
    // server took the idempotent path (already in the requested state) or
    // another client changed something in the same window, and in both cases
    // the reply is right and our arithmetic is not.
    m_held.clear();
    for (const QString& id : roleIds) m_held.insert(id);
    rebuild();
}

void SelfRoleModel::onSelfRoleFailed(const QString& roleId, int status,
                                      const QString& message)
{
    const bool wasPending = m_pending.remove(roleId) > 0;

    QString name = roleId;
    for (const Row& r : m_all) {
        if (r.id == roleId) { name = r.name; break; }
    }

    // The three refusals a member can actually provoke, worded as what
    // happened rather than as an HTTP code. 403 is the interesting one: it is
    // what the containment re-check returns, and it is not a bug report — it
    // means an administrator narrowed @everyone since this role was published.
    QString text;
    if (status == 403) {
        text = tr("The server would not change “%1”: %2").arg(name, message);
    } else if (status == 404) {
        text = tr("“%1” no longer exists on this server.").arg(name);
    } else if (message.isEmpty()) {
        text = tr("Couldn't change “%1”. Try again.").arg(name);
    } else {
        text = tr("Couldn't change “%1” — %2").arg(name, message);
    }
    setErrorText(text);

    // Back to the last known truth. Note this is NOT "undo the optimism": if a
    // sync landed while the request was in flight, m_held already moved, and
    // the honest thing is to show where the member actually stands rather than
    // where they stood when they clicked.
    if (wasPending) rebuild();
}

void SelfRoleModel::dismissError()
{
    setErrorText(QString());
}

void SelfRoleModel::setErrorText(const QString& text)
{
    if (m_errorText == text) return;
    m_errorText = text;
    emit errorTextChanged();
}

void SelfRoleModel::reset()
{
    m_all.clear();
    m_everyone = 0;
    m_loaded = false;
    m_held.clear();
    // Dropped, not left pending: the replies these were waiting for belong to
    // a connection that is gone, and a row stuck mid-flight forever is worse
    // than one that snaps back to nothing.
    m_pending.clear();
    setErrorText(QString());
    rebuild();
}
