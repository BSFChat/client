#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <cstdint>
#include <functional>

// View-model behind the member-facing self-assignable role picker.
//
// The server half of this shipped as `ServerRole.self_assignable` plus
// PUT/DELETE /_matrix/client/v3/bsfchat/self_roles/{id}: a role any member may
// add to or remove from themselves, with no MANAGE_ROLES and no rank check.
// The TibiaGuru bot publishes one such role per boss and treats HOLDING the
// role as the subscription, so this picker is the subscription UI — there is
// no per-user table behind it to read.
//
// It lives apart from ServerConnection for the reason BotAdminModel and
// PermissionMath give: everything interesting here is a small state machine
// over replies that can arrive out of order or contradict what the UI already
// drew, and ServerConnection cannot be instantiated in a unit test. The
// network is reached through `Hooks` (std::function), so tests/test_self_roles
// substitutes a recording fake and delivers the replies by hand.
//
// ─────────────────────── what may be offered, and why ───────────────────────
//
// `self_assignable` is necessary but NOT sufficient. The server's containment
// rule (server/src/auth/Permissions.cpp) says a self-assignable role may not
// grant a permission @everyone does not already have, and it enforces that
// TWICE: once when the role document is stored, and again — against the
// document as it stands — at claim time. The two come apart the moment an
// admin narrows @everyone after an opt-in role was created, and when they do,
// PermissionsEngine::may_self_assign_role starts refusing a role that is still
// flagged `self_assignable` on the wire.
//
// So this model re-runs the containment arithmetic client-side and marks such
// a role `blocked`. That is a courtesy, not a gate: the server's refusal is
// the authority and onSelfRoleFailed() is what honours it. The mirror only
// exists so the common case does not require a round trip to learn that a
// button was a trap.
//
// A blocked role the member does NOT hold is dropped from the list entirely —
// the brief for this picker is that it never shows a role a member cannot
// take. A blocked role the member DOES hold is kept, ticked and locked, with
// its reason: hiding it would make a role they are visibly wearing disappear
// from the one screen that explains where it came from, and they are genuinely
// stuck with it (the server gates removal through the SAME check as addition,
// so a member cannot shed a role that has drifted out of containment).
//
// ────────────────────────── admin-granted roles ──────────────────────────
//
// There is no such thing as "self-assignable state" distinct from the role
// assignment itself. bsfchat.member.roles is a flat list of ids with no
// provenance, and change_self_role() erases the id without asking how it got
// there. A self-assignable role placed on a member by an administrator is
// therefore indistinguishable from one they picked, and the server will let
// them drop it. This model reflects that rather than inventing a lock the
// server would not honour: a held, unblocked, self-assignable role is shown
// ticked and is untickable, whoever put it there.
class SelfRoleModel : public QObject {
    Q_OBJECT

    // One entry per offered role, as QVariantMaps:
    //   { id, name, color, held, pending, blocked, blockedReason }
    // Already filtered and ordered — QML renders it as-is and decides nothing.
    Q_PROPERTY(QVariantList roles READ roles NOTIFY rolesChanged)
    // True while any toggle is in flight. The picker keeps working (each row
    // guards itself) but the footer uses this to say so.
    Q_PROPERTY(bool busy READ busy NOTIFY rolesChanged)
    // True once a role document has been seen at all. Distinguishes "this
    // server publishes no opt-in roles" (empty state) from "we have not
    // sync'd yet" (say nothing), which look identical from an empty list.
    Q_PROPERTY(bool loaded READ loaded NOTIFY rolesChanged)
    // Last refusal, empty when nothing has failed since it was dismissed.
    // Rendered inline next to the list rather than as a toast, so the reason a
    // checkbox sprang back sits where the checkbox is.
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit SelfRoleModel(QObject* parent = nullptr);

    // The network seam. ServerConnection installs the real calls; the test
    // records them. An unset hook makes the toggle a no-op rather than a
    // crash — which is what a picker opened against a dead connection should
    // do — but it also leaves the row un-pending, so nothing hangs.
    struct Hooks {
        std::function<void(const QString& roleId)> addSelfRole;
        std::function<void(const QString& roleId)> removeSelfRole;
    };
    Hooks hooks;

    QVariantList roles() const { return m_view; }
    bool busy() const { return !m_pending.isEmpty(); }
    bool loaded() const { return m_loaded; }
    QString errorText() const { return m_errorText; }

    // Fed from ServerConnection on every server.roles / member.roles change.
    // `serverRoles` is the raw role document; `heldRoleIds` is OUR OWN
    // bsfchat.member.roles list. Rows with a request in flight keep their
    // optimistic tick until their own reply lands — a sync arriving mid-flight
    // is not news about the write we are waiting for, and adopting it would
    // make the checkbox flicker back and then forward again.
    void setServerState(const QJsonArray& serverRoles, const QStringList& heldRoleIds);

    // ── actions (called from QML) ──────────────────────────────────────────

    // Flip `roleId`. Ignored for an unknown, blocked or already-in-flight row.
    Q_INVOKABLE void toggle(const QString& roleId);
    Q_INVOKABLE void dismissError();

    // ── replies ────────────────────────────────────────────────────────────

    // A 200 from either endpoint. `roleIds` is the server's authoritative
    // post-change list and REPLACES our idea of what is held — including when
    // it disagrees with the optimistic tick, which is exactly what the
    // idempotent no-op reply looks like.
    void onSelfRoleResult(const QString& roleId, const QStringList& roleIds);
    // A refusal. The row returns to the last known truth — not to the opposite
    // of what was asked, which is the same thing only when the optimism was
    // the only change since.
    void onSelfRoleFailed(const QString& roleId, int status, const QString& message);

    // Connection dropped / server switched.
    void reset();

signals:
    void rolesChanged();
    void errorTextChanged();

private:
    struct Row {
        QString id;
        QString name;
        QString color;
        std::uint64_t permissions = 0;
        bool selfAssignable = false;
    };

    void rebuild();
    void setErrorText(const QString& text);
    bool effectiveHeld(const QString& roleId) const;

    // Every role in the document, in document order, parsed once.
    QVector<Row> m_all;
    // Permissions of @everyone in that same document: the containment ceiling.
    std::uint64_t m_everyone = 0;
    bool m_loaded = false;

    // Authoritative: our own bsfchat.member.roles.
    QSet<QString> m_held;
    // roleId -> the value we optimistically drew while its request is in
    // flight. Presence in this map IS "pending".
    QHash<QString, bool> m_pending;

    QVariantList m_view;
    QString m_errorText;
};
