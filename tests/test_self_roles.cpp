// The member-facing self-assignable role picker.
//
// Two halves, in the spirit of test_bots.cpp:
//
//   1. SelfRoleModel driven headlessly. It reaches the network through
//      std::function hooks, so the whole picker — what it offers, what it
//      does while a request is in flight, and what it does when the server
//      says no — is exercised with a recording fake and no event loop, no
//      network and no GUI.
//
//   2. Source-text scans for the things no headless test can reach: that the
//      picker is wired to a surface an ordinary member can actually open, and
//      that the QML does not quietly re-implement the filtering the model is
//      responsible for.
//
// The server's rules this pins against (server/src/auth/Permissions.cpp,
// server/src/api/RoleHandler.cpp):
//
//   * may_self_assign_role refuses @everyone, refuses a role that is not
//     flagged self_assignable, and refuses a role whose permissions are not a
//     subset of @everyone's AS THE DOCUMENT STANDS — re-checked at claim time,
//     not trusted from the write that created the role.
//   * The same check gates ADDITION and REMOVAL. There is no separate
//     "may_self_unassign".
//   * Both endpoints are idempotent and both reply with the member's whole
//     post-change role_ids list.
//   * bsfchat.member.roles carries no provenance, so a role an ADMIN put on a
//     member is indistinguishable from one they picked, and change_self_role
//     will happily erase it.

#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>

#include "model/SelfRoleModel.h"

namespace {

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Comments only; string literals are left alone. Can only remove text, so a
// scan below may miss an offender but cannot invent one. Same helper, same
// caveat, as test_qml_hygiene.
QString withoutComments(QString src)
{
    static const QRegularExpression block(QStringLiteral(R"(/\*.*?\*/)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

// A role document entry. `perms` goes in as a hex string, which is the shape
// the server actually sends; one case below sends a number instead.
QJsonObject role(const QString& id, const QString& perms, bool selfAssignable,
                 const QString& name = QString(), const QString& color = QString())
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name.isEmpty() ? id : name;
    o["color"] = color.isEmpty() ? QStringLiteral("#5865f2") : color;
    o["permissions"] = perms;
    o["self_assignable"] = selfAssignable;
    return o;
}

// @everyone as BSFChat ships it: view + send + attach + embed, change
// nickname, add reactions.
QJsonObject everyone(const QString& perms = QStringLiteral("0x480f"))
{
    return role(QStringLiteral("everyone"), perms, false, QStringLiteral("@everyone"));
}

QVariantMap rowFor(const SelfRoleModel& m, const QString& id)
{
    for (const QVariant& v : m.roles()) {
        const auto row = v.toMap();
        if (row.value(QStringLiteral("id")).toString() == id) return row;
    }
    return {};
}

QStringList offeredIds(const SelfRoleModel& m)
{
    QStringList out;
    for (const QVariant& v : m.roles()) {
        out << v.toMap().value(QStringLiteral("id")).toString();
    }
    return out;
}

// Records what the model asked the network to do.
struct Recorder {
    QStringList added;
    QStringList removed;

    void install(SelfRoleModel& m)
    {
        m.hooks.addSelfRole = [this](const QString& id) { added << id; };
        m.hooks.removeSelfRole = [this](const QString& id) { removed << id; };
    }
};

} // namespace

class SelfRolesTest : public QObject {
    Q_OBJECT
private slots:

    // ─────────────── 1. what is on offer ───────────────

    void onlySelfAssignableRolesAreOffered()
    {
        SelfRoleModel m;
        m.setServerState(QJsonArray{
            everyone(),
            role("mod", "0x0010", false),        // a real role; not opt-in
            role("boss-ferumbras", "0x0000", true),
            role("boss-ghazbaran", "0x0000", true),
        }, {});

        QCOMPARE(offeredIds(m), QStringList({"boss-ferumbras", "boss-ghazbaran"}));
        QVERIFY(m.loaded());
    }

    void theEveryoneRoleIsNeverOffered()
    {
        // The server refuses to store @everyone as self-assignable at all, so
        // this can only arrive from a legacy or hand-edited document. If it
        // ever did and the picker honoured it, the row would advertise a
        // "leave @everyone" button — and the member who clicked it would drop
        // out of every permission the server grants by default.
        SelfRoleModel m;
        QJsonObject rogue = everyone();
        rogue["self_assignable"] = true;
        m.setServerState(QJsonArray{rogue, role("opt", "0x0000", true)},
                         {"everyone"});

        QCOMPARE(offeredIds(m), QStringList({"opt"}));
    }

    void loadedIsFalseBeforeARoleDocumentArrives()
    {
        // An empty list means two different things and the picker words its
        // empty state differently for each. Before any sync there is nothing
        // to say; after one, "this server publishes none" is the answer.
        SelfRoleModel m;
        QVERIFY(!m.loaded());
        m.setServerState(QJsonArray{}, {});
        QVERIFY(!m.loaded());
        m.setServerState(QJsonArray{everyone()}, {});
        QVERIFY(m.loaded());
        QVERIFY(m.roles().isEmpty());
    }

    void aPermissionBitfieldParsesAsHexStringOrNumber()
    {
        // The server sends "0x480f"; our own optimistic writes put a number
        // in the same array. Reading one shape and not the other would make
        // the containment arithmetic come out zero, which fails OPEN — every
        // role would look contained and every row would be offered.
        SelfRoleModel m;
        QJsonObject everyoneNumeric;
        everyoneNumeric["id"] = "everyone";
        everyoneNumeric["name"] = "@everyone";
        everyoneNumeric["permissions"] = 0x480f;
        everyoneNumeric["self_assignable"] = false;

        QJsonObject overreaching;
        overreaching["id"] = "sneaky";
        overreaching["name"] = "sneaky";
        overreaching["permissions"] = 0x0010; // MANAGE_MESSAGES; not in @everyone
        overreaching["self_assignable"] = true;

        m.setServerState(QJsonArray{everyoneNumeric, overreaching}, {});
        QVERIFY2(offeredIds(m).isEmpty(),
                 "a numeric bitfield beyond @everyone was treated as contained");
    }

    // ─────────────── 2. the containment rule ───────────────

    void aRoleBeyondEveryonesPermissionsIsNotOffered()
    {
        SelfRoleModel m;
        m.setServerState(QJsonArray{
            everyone(),
            // Flagged self-assignable, but grants MANAGE_MESSAGES. The server
            // would 403 this at claim time.
            role("overreach", "0x0010", true),
            role("fine", "0x0001", true),
        }, {});

        QCOMPARE(offeredIds(m), QStringList({"fine"}));
    }

    void aBlockedRoleTheMemberAlreadyHoldsIsShownLockedWithItsReason()
    {
        // The drift case the server's comment describes: @everyone is
        // narrowed AFTER an opt-in role was published. The role is still
        // flagged self_assignable and the member is still wearing it, but the
        // claim-time check now refuses it — and because removal is gated by
        // the SAME check, they cannot take it off either. Hiding the row
        // would make a role they can see on their own name vanish from the
        // one screen that explains it.
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);

        m.setServerState(QJsonArray{
            everyone("0x0001"),            // narrowed to VIEW_CHANNEL only
            role("was-fine", "0x0003", true),
        }, {"was-fine"});

        const auto row = rowFor(m, "was-fine");
        QVERIFY2(!row.isEmpty(), "a blocked role the member holds was hidden");
        QCOMPARE(row.value("held").toBool(), true);
        QCOMPARE(row.value("blocked").toBool(), true);
        QVERIFY2(!row.value("blockedReason").toString().isEmpty(),
                 "a locked row must say why");

        // And the model declines to send a request it knows will be refused.
        m.toggle("was-fine");
        QVERIFY(rec.added.isEmpty());
        QVERIFY(rec.removed.isEmpty());
        QCOMPARE(rowFor(m, "was-fine").value("held").toBool(), true);
    }

    // ─────────────── 3. optimistic, and honest ───────────────

    void tickingARoleSendsTheAddCallAndShowsItAtOnce()
    {
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});

        QSignalSpy changed(&m, &SelfRoleModel::rolesChanged);
        m.toggle("boss");

        QCOMPARE(rec.added, QStringList({"boss"}));
        QVERIFY(rec.removed.isEmpty());
        QVERIFY(changed.count() > 0);

        const auto row = rowFor(m, "boss");
        QCOMPARE(row.value("held").toBool(), true);
        QCOMPARE(row.value("pending").toBool(), true);
        QVERIFY(m.busy());
    }

    void theServersReplyIsAdoptedWholesaleRatherThanOurOptimism()
    {
        // The idempotent path. The member's assignment already contained the
        // role (an admin granted it a moment ago and the sync had not landed),
        // so the server writes nothing and answers with the list as it stands
        // — which does NOT match what our tick assumed. The reply wins.
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});

        m.toggle("boss");
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), true);

        // Server: "you do not have it, and I did not add it" is impossible
        // here, but "you have @everyone and nothing else" is exactly what a
        // reply looks like when another client removed it in the same window.
        m.onSelfRoleResult("boss", QStringList({"everyone"}));

        const auto row = rowFor(m, "boss");
        QCOMPARE(row.value("held").toBool(), false);
        QCOMPARE(row.value("pending").toBool(), false);
        QVERIFY(!m.busy());
    }

    void aRefusalReturnsTheRowToTruthAndSaysWhy()
    {
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true,
                                                      "Ferumbras")}, {});

        m.toggle("boss");
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), true);

        QSignalSpy err(&m, &SelfRoleModel::errorTextChanged);
        m.onSelfRoleFailed("boss", 403,
                           QStringLiteral("That role grants permissions beyond "
                                          "@everyone and cannot be self-assigned"));

        const auto row = rowFor(m, "boss");
        QCOMPARE(row.value("held").toBool(), false);
        QCOMPARE(row.value("pending").toBool(), false);
        QCOMPARE(err.count(), 1);
        QVERIFY2(m.errorText().contains(QStringLiteral("Ferumbras")),
                 "the refusal must name the role, not its id");
        QVERIFY2(m.errorText().contains(QStringLiteral("beyond")),
                 "the server's own reason must survive to the user");
    }

    void aRefusalReturnsToTheNEWTruthWhenASyncMovedItMidFlight()
    {
        // Not "undo the optimism" — that is only the same thing when nothing
        // else changed. Here the member had the role removed by an admin
        // while their own add was in flight; the add is then refused. The
        // honest end state is where they actually stand, which the sync
        // already told us.
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        const QJsonArray doc{everyone(), role("boss", "0x0000", true)};

        m.setServerState(doc, {"boss"});
        m.toggle("boss");                       // asks to REMOVE it
        QCOMPARE(rec.removed, QStringList({"boss"}));
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), false);

        // Sync: an admin removed it first.
        m.setServerState(doc, {});
        // Our pending row keeps its optimistic value — which happens to agree.
        QCOMPARE(rowFor(m, "boss").value("pending").toBool(), true);

        m.onSelfRoleFailed("boss", 403, QStringLiteral("nope"));
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), false);
    }

    void aSyncArrivingMidFlightDoesNotFlickerThePendingRow
        ()
    {
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        const QJsonArray doc{everyone(), role("boss", "0x0000", true)};
        m.setServerState(doc, {});

        m.toggle("boss");
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), true);

        // A sync for an unrelated change re-pushes the same (stale) held set.
        // Adopting it would tick the box back off and then on again when the
        // reply lands.
        m.setServerState(doc, {});
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), true);
        QCOMPARE(rowFor(m, "boss").value("pending").toBool(), true);

        m.onSelfRoleResult("boss", QStringList({"everyone", "boss"}));
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), true);
        QCOMPARE(rowFor(m, "boss").value("pending").toBool(), false);
    }

    void aSecondClickWhileTheFirstIsInFlightIsIgnored()
    {
        // Two writes racing to the same list leaves whichever reply landed
        // last as the truth, which is not what the second click asked for.
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});

        m.toggle("boss");
        m.toggle("boss");
        m.toggle("boss");

        QCOMPARE(rec.added, QStringList({"boss"}));
        QVERIFY(rec.removed.isEmpty());
    }

    // ─────────────── 4. admin-granted roles ───────────────

    void anAdminGrantedRoleIsTickedAndTheMemberMayDropIt()
    {
        // The server keeps no provenance: bsfchat.member.roles is a flat list
        // of ids, and change_self_role erases the id without asking who put it
        // there. So there is nothing to lock, and pretending otherwise would
        // be a client-side fiction any other client could walk straight
        // through. If that is ever wanted, the SERVER has to say so.
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)},
                         {"everyone", "boss"});

        const auto row = rowFor(m, "boss");
        QCOMPARE(row.value("held").toBool(), true);
        QCOMPARE(row.value("blocked").toBool(), false);

        m.toggle("boss");
        QCOMPARE(rec.removed, QStringList({"boss"}));
    }

    void aHeldRoleThatIsNotSelfAssignableIsNotInThePickerAtAll()
    {
        // The other half of the same story. An admin-granted "mod" is not
        // shown, because the server would refuse to remove it — showing it
        // ticked would advertise a tick the member cannot untick.
        SelfRoleModel m;
        m.setServerState(QJsonArray{
            everyone(),
            role("mod", "0x0010", false),
            role("boss", "0x0000", true),
        }, {"everyone", "mod", "boss"});

        QCOMPARE(offeredIds(m), QStringList({"boss"}));
    }

    // ─────────────── 5. housekeeping ───────────────

    void anUnsetHookLeavesNothingPending()
    {
        // A picker opened against a disconnected server must not leave a row
        // spinning forever waiting for a reply nobody will send.
        SelfRoleModel m;
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});
        m.toggle("boss");

        QCOMPARE(rowFor(m, "boss").value("pending").toBool(), false);
        QCOMPARE(rowFor(m, "boss").value("held").toBool(), false);
        QVERIFY(!m.busy());
    }

    void resetClearsEverythingIncludingRequestsInFlight()
    {
        SelfRoleModel m;
        Recorder rec;
        rec.install(m);
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});
        m.toggle("boss");
        m.onSelfRoleFailed("boss", 500, QStringLiteral("boom"));
        QVERIFY(!m.errorText().isEmpty());

        m.reset();
        QVERIFY(m.roles().isEmpty());
        QVERIFY(!m.loaded());
        QVERIFY(!m.busy());
        QVERIFY(m.errorText().isEmpty());
    }

    void dismissingTheErrorClearsIt()
    {
        SelfRoleModel m;
        m.setServerState(QJsonArray{everyone(), role("boss", "0x0000", true)}, {});
        m.onSelfRoleFailed("boss", 404, QString());
        QVERIFY(!m.errorText().isEmpty());
        m.dismissError();
        QVERIFY(m.errorText().isEmpty());
    }

    // ─────────────── 6. the surface is reachable, and thin ───────────────

    void thePickerIsReachableWithoutAnyPermission()
    {
        // The whole point. ServerSettings.qml is the wrong home precisely
        // because its gear is rendered only for a member holding one of four
        // admin permissions — pin that the picker's entry point is NOT behind
        // that gate, and that the gate it replaced is still there (so this
        // test keeps meaning something if the gear's condition moves).
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ChannelList.qml")));
        QVERIFY2(!src.isEmpty(), "ChannelList.qml not found");

        static const QRegularExpression gearGate(
            QStringLiteral(R"(canManageRoles\([^)]*\)\s*\|\|\s*\w+\.canManageChannel)"));
        QVERIFY2(gearGate.match(src).hasMatch(),
                 "the Server Settings gear is no longer gated on admin "
                 "permissions — this guard assumed it was");

        const qsizetype at = src.indexOf(QStringLiteral("openSelfRoles()"));
        QVERIFY2(at > 0, "nothing in ChannelList opens the self-role picker");

        // The menu item must not sit inside the admin-gated block. Checked by
        // distance rather than by parsing QML: the gear's gate and its button
        // are within ~40 lines of each other near the top of the header, and
        // the user menu is ~1300 lines further down.
        const qsizetype gearAt = gearGate.match(src).capturedStart();
        QVERIFY2(at > gearAt + 2000,
                 "the self-role entry is next to the admin-gated gear; it must "
                 "be somewhere every member can reach");

        // And it must be the user menu, which has no permission condition.
        const qsizetype menuAt = src.indexOf(QStringLiteral("openUserSettings()"));
        QVERIFY2(menuAt > 0 && qAbs(at - menuAt) < 2000,
                 "the self-role entry is not beside the other user-menu items");
    }

    void thePickerDoesNotReimplementTheModelsFiltering()
    {
        // Every decision about which roles are on offer is the model's, and
        // the model is the only half a headless test can drive. A filter
        // written in QML — reading self_assignable, or the permission
        // bitfield, or serverRoles directly — is a filter nothing here can
        // check, and it is exactly where a "shows a role you cannot take" bug
        // would reappear.
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/SelfRolePicker.qml")));
        QVERIFY2(!src.isEmpty(), "SelfRolePicker.qml not found");

        QVERIFY2(!src.contains(QStringLiteral("self_assignable")),
                 "the picker reads self_assignable itself instead of "
                 "rendering the model's list");
        QVERIFY2(!src.contains(QStringLiteral("serverRoles")),
                 "the picker reads the raw role document instead of the "
                 "model's filtered list");
        QVERIFY2(!src.contains(QStringLiteral("permissions")),
                 "the picker does its own permission arithmetic");
        QVERIFY2(src.contains(QStringLiteral("selfRoleModel")),
                 "the picker is not bound to the model at all");
        QVERIFY2(src.contains(QStringLiteral(".toggle(")),
                 "the picker never calls toggle()");
    }

    void theCheckboxNeverLatchesAheadOfTheModel()
    {
        // A CheckBox toggles itself on click. Left alone it would hold that
        // state whatever the server answered, and the row would silently
        // diverge from the truth — the one failure mode this feature was
        // specified against. The handler must put the tick back where the
        // model says it belongs before asking for the change.
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/SelfRolePicker.qml")));
        QVERIFY(!src.isEmpty());

        const qsizetype at = src.indexOf(QStringLiteral("onToggled:"));
        QVERIFY2(at > 0, "the picker's checkbox has no onToggled handler");
        const QString handler = src.mid(at, 220);
        QVERIFY2(handler.contains(QStringLiteral("checked = ")),
                 "onToggled does not reassert the model's value, so the "
                 "checkbox latches its own state");
    }

    // ─────────────── 7. the admin half, and the mirror ───────────────

    void theRoleEditorCanPublishASelfAssignableRole()
    {
        // Before this change the editor CARRIED the flag through a save but
        // had no control to set it, so the only way to publish an opt-in role
        // was to PATCH it by hand — which made the picker unreachable on any
        // server without a bot or a curl.
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ServerSettings.qml")));
        QVERIFY2(!src.isEmpty(), "ServerSettings.qml not found");

        QVERIFY2(src.contains(QStringLiteral("roleScratchSelfAssignable")),
                 "the role editor has no self-assignable control");
        // And the save must write the scratch, not a carried-through copy —
        // otherwise the checkbox moves nothing.
        static const QRegularExpression save(
            QStringLiteral(R"(self_assignable:\s*serverSettingsPopup\.roleScratchSelfAssignable)"));
        QVERIFY2(save.match(src).hasMatch(),
                 "the role save does not write the self-assignable scratch "
                 "value, so the checkbox is decorative");
    }

    void selfAssignableIsNotAPermissionBitAndIsNotInThePermissionGrid()
    {
        // The brief's question, answered mechanically. test_bots.cpp
        // reconstructs the editor's mask from the `permissionFlags` block and
        // requires it to equal the client's mirror of every bit the server
        // enforces. self_assignable is a property of the ROLE, not of who
        // holds it, and has no bit — so it must sit OUTSIDE that block, or it
        // would be parsed as a flag and break the mask comparison.
        const QString src = readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ServerSettings.qml"));
        QVERIFY(!src.isEmpty());

        const qsizetype start = src.indexOf(QStringLiteral("readonly property var permissionFlags:"));
        QVERIFY2(start > 0, "permissionFlags list not found");
        const qsizetype end = src.indexOf(QStringLiteral("\n    ]"), start);
        QVERIFY(end > start);
        const QString block = src.mid(start, end - start);

        QVERIFY2(!block.contains(QStringLiteral("self_assignable")),
                 "self_assignable is inside the permission grid; it is not a "
                 "permission bit and test_bots' mask reconstruction will "
                 "parse it as one");
        QVERIFY2(!block.contains(QStringLiteral("roleScratchSelfAssignable")),
                 "the self-assignable control is inside the permission grid");
        // Same rule the mentionable flag already follows.
        QVERIFY2(!block.contains(QStringLiteral("mentionable")),
                 "mentionable drifted into the permission grid");
    }
};

QTEST_MAIN(SelfRolesTest)
#include "test_self_roles.moc"
