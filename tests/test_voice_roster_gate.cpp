// Call-invite roster gate (V-8): only voice-channel members get an
// answer, a join that races its own m.call.member is held briefly
// instead of refused, and a held peer that leaves the roster is dropped.

#include <QtTest>

#include "voice/VoiceRosterGate.h"

using voice::InviteGate;

namespace {
const QString kAlice = QStringLiteral("@alice:example.org");
const QString kBob = QStringLiteral("@bob:example.org");
const QString kEve = QStringLiteral("@eve:example.org");
}

class TestVoiceRosterGate : public QObject {
    Q_OBJECT
private slots:
    void acceptsRosterMember() {
        QCOMPARE(voice::gateInvite({kAlice, kBob}, kAlice, 1000, 1000),
                 InviteGate::Accept);
    }

    void holdsNonMemberDuringGrace() {
        QCOMPARE(voice::gateInvite({kAlice}, kEve, 1000, 1000), InviteGate::Hold);
        QCOMPARE(voice::gateInvite({kAlice}, kEve, 1000,
                                   1000 + voice::kInviteRosterGraceMs - 1),
                 InviteGate::Hold);
    }

    void rejectsNonMemberAfterGrace() {
        QCOMPARE(voice::gateInvite({kAlice}, kEve, 1000,
                                   1000 + voice::kInviteRosterGraceMs),
                 InviteGate::Reject);
        // Re-inviting does not restart the clock: the caller keeps the
        // first arrival time while an invite is held.
        QCOMPARE(voice::gateInvite({}, kEve, 0, 10'000), InviteGate::Reject);
    }

    void racingJoinIsAcceptedOnceOnRoster() {
        // Held at t=0, sender appears at t=1 s: accepted.
        QCOMPARE(voice::gateInvite({}, kBob, 0, 0), InviteGate::Hold);
        QCOMPARE(voice::gateInvite({kBob}, kBob, 0, 1000), InviteGate::Accept);
    }

    void emptySenderNeverAccepted() {
        QCOMPARE(voice::gateInvite({QString()}, QString(), 0, 10'000),
                 InviteGate::Reject);
    }

    void dropsPeerThatLeftRoster() {
        const auto u = voice::applyRosterUpdate({kAlice, kBob}, {kAlice},
                                                {kAlice, kBob}, {});
        QCOMPARE(u.toDrop, QStringList{kBob});
        QVERIFY(u.toAdmit.isEmpty());
    }

    void keepsPeerNeverSeenOnRoster() {
        // Offered from the poll before its m.call.member arrived: not a
        // departure, so not dropped.
        const auto u = voice::applyRosterUpdate({kAlice}, {kAlice},
                                                {kAlice, kBob}, {});
        QVERIFY(u.toDrop.isEmpty());
    }

    void admitsHeldInviterWhoJoined() {
        const auto u = voice::applyRosterUpdate({kAlice}, {kAlice, kBob},
                                                {kAlice}, {kBob, kEve});
        QCOMPARE(u.toAdmit, QStringList{kBob});
        QVERIFY(u.toDrop.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestVoiceRosterGate)
#include "test_voice_roster_gate.moc"
