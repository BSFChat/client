// Transport selection and mixed-mode detection.
//
// The rule under test: a voice channel is all-mesh or all-SFU. Violating
// it does not produce an error — it produces a call where a mesh client
// waits forever for an m.call.invite an SFU participant will never send,
// while everyone's member list shows all present. Silent partial audio.
//
// Not hypothetical. Android has no LiveKit SDK and stays on mesh; desktop
// moves to LiveKit. One Android user plus one desktop user in the same
// voice channel is the ordinary case, so every branch here is a shape
// production will actually hit.
//
// Pure logic: no transport instances, no network, no audio device, no
// server. That is the point of putting the decision in its own unit.

#include <QtTest>

#include "voice/VoiceTransportSelector.h"

using namespace voice;

namespace {

// Build one GET .../voice/members row.
QJsonObject member(const QString& userId, bool active,
                   const QString& transport = QString()) {
    QJsonObject o;
    o[QStringLiteral("user_id")] = userId;
    o[QStringLiteral("active")] = active;
    if (!transport.isNull()) {
        o[QStringLiteral("transport")] = transport;
    }
    return o;
}

QJsonArray roster(std::initializer_list<QJsonObject> rows) {
    QJsonArray a;
    for (const QJsonObject& r : rows) {
        a.append(r);
    }
    return a;
}

// A desktop client: has the SDK, and the server issued a token.
TransportInputs desktop(const QJsonArray& members = {}) {
    TransportInputs in;
    in.clientSupportsLiveKit = true;
    in.serverOfferedLiveKitToken = true;
    in.voiceMembers = members;
    in.localUserId = QStringLiteral("@me:example.org");
    return in;
}

// An Android client: no SDK for the platform, so mesh regardless of what
// the server offers.
TransportInputs android(const QJsonArray& members = {}) {
    TransportInputs in;
    in.clientSupportsLiveKit = false;
    in.serverOfferedLiveKitToken = true;
    in.voiceMembers = members;
    in.localUserId = QStringLiteral("@me:example.org");
    return in;
}

} // namespace

class TestVoiceTransport : public QObject {
    Q_OBJECT

private slots:
    // ---- Roster classification -----------------------------------

    void emptyRosterIsEmpty() {
        const RosterTransports r = classifyRoster({}, QStringLiteral("@me:example.org"));
        QVERIFY(r.empty());
        QCOMPARE(r.meshTotal(), 0);
        QCOMPARE(r.livekit, 0);
    }

    // Every client deployed today writes no `transport` field. If an
    // unlabelled row counted as anything but mesh, the first joiner into
    // an existing call would pick the SFU and go silently deaf to
    // everyone already in it.
    void unlabelledMemberCountsAsMesh() {
        const RosterTransports r = classifyRoster(
            roster({member(QStringLiteral("@a:example.org"), true)}),
            QStringLiteral("@me:example.org"));
        QCOMPARE(r.unlabelled, 1);
        QCOMPARE(r.mesh, 0);
        QCOMPARE(r.meshTotal(), 1);
        QCOMPARE(r.livekit, 0);
    }

    // Likewise for a value we do not recognise — a newer or buggy client
    // writing "sfu" or "webrtc" must not be read as "not mesh".
    void unrecognisedTransportValueCountsAsMesh() {
        const RosterTransports r = classifyRoster(
            roster({member(QStringLiteral("@a:example.org"), true,
                           QStringLiteral("quic-magic"))}),
            QStringLiteral("@me:example.org"));
        QCOMPARE(r.meshTotal(), 1);
        QCOMPARE(r.livekit, 0);
    }

    void labelledTransportsAreCountedSeparately() {
        const RosterTransports r = classifyRoster(
            roster({member(QStringLiteral("@a:example.org"), true,
                           QLatin1String(kTransportMesh)),
                    member(QStringLiteral("@b:example.org"), true,
                           QLatin1String(kTransportLiveKit)),
                    member(QStringLiteral("@c:example.org"), true,
                           QLatin1String(kTransportLiveKit))}),
            QStringLiteral("@me:example.org"));
        QCOMPARE(r.mesh, 1);
        QCOMPARE(r.livekit, 2);
        QVERIFY(!r.empty());
    }

    // A crashed client leaves `active: true` behind only until the ghost
    // reaper flips it. Once flipped, that row must stop pinning the
    // channel — otherwise one Android crash makes a channel mesh-only
    // until someone restarts the server.
    void inactiveMembersAreIgnored() {
        const RosterTransports r = classifyRoster(
            roster({member(QStringLiteral("@a:example.org"), false,
                           QLatin1String(kTransportMesh)),
                    member(QStringLiteral("@b:example.org"), false,
                           QLatin1String(kTransportLiveKit))}),
            QStringLiteral("@me:example.org"));
        QVERIFY(r.empty());
    }

    // Our own stale row — from a reaped session, or from this user's
    // other device — must not veto our own join.
    void ownRowIsExcluded() {
        const RosterTransports r = classifyRoster(
            roster({member(QStringLiteral("@me:example.org"), true,
                           QLatin1String(kTransportMesh))}),
            QStringLiteral("@me:example.org"));
        QVERIFY(r.empty());
    }

    // Only OUR row is excluded, and the comparison must be exact.
    // Matrix user-id localparts are case-sensitive, so "@ME:example.org"
    // is a different user. A case-insensitive compare here would drop a
    // real participant from the roster and let this client pick the
    // wrong transport — the exact mixed-mode failure, arrived at by a
    // string-comparison slip.
    void onlyAnExactLocalUserIdMatchIsExcluded() {
        const QString me = QStringLiteral("@me:example.org");
        for (const QString& other : {QStringLiteral("@ME:example.org"),
                                     QStringLiteral("@me2:example.org"),
                                     QStringLiteral("@me:example.org.uk"),
                                     QStringLiteral("@m:example.org")}) {
            const RosterTransports r = classifyRoster(
                roster({member(other, true, QLatin1String(kTransportLiveKit))}), me);
            QVERIFY2(r.livekit == 1,
                     qPrintable(QStringLiteral("roster row %1 was wrongly "
                                               "treated as our own").arg(other)));
        }
    }

    // With no local user id known, nothing is excluded — otherwise an
    // unset id would silently empty the roster and every client would
    // think it was first in and free to pick LiveKit.
    void emptyLocalUserIdExcludesNobody() {
        const RosterTransports r = classifyRoster(
            roster({member(QString(), true, QLatin1String(kTransportLiveKit)),
                    member(QStringLiteral("@a:example.org"), true,
                           QLatin1String(kTransportMesh))}),
            QString());
        QCOMPARE(r.livekit, 1);
        QCOMPARE(r.mesh, 1);
    }

    void malformedRowsAreSkipped() {
        QJsonArray a;
        a.append(QJsonValue(42));
        a.append(QJsonValue(QStringLiteral("not-an-object")));
        // An object with no `active` key is malformed, not implicitly live.
        QJsonObject noActive;
        noActive[QStringLiteral("user_id")] = QStringLiteral("@a:example.org");
        a.append(noActive);
        const RosterTransports r = classifyRoster(a, QStringLiteral("@me:example.org"));
        QVERIFY(r.empty());
    }

    // ---- The decision --------------------------------------------

    void desktopIntoEmptyChannelPicksLiveKit() {
        QVERIFY(chooseTransport(desktop()).isLiveKit());
    }

    void androidIntoEmptyChannelPicksMesh() {
        QVERIFY(chooseTransport(android()).isMesh());
    }

    // The headline case: Android is already in the call, desktop joins.
    // Desktop CAN fall back, so it must, silently and without ceremony.
    void desktopJoiningAnAndroidCallFallsBackToMesh() {
        const TransportDecision d = chooseTransport(desktop(
            roster({member(QStringLiteral("@android:example.org"), true,
                           QLatin1String(kTransportMesh))})));
        QVERIFY(d.isMesh());
        QVERIFY(d.refusalReason.isEmpty());
    }

    // Same, against a roster written by today's clients (no field).
    void desktopJoiningAnUnlabelledCallFallsBackToMesh() {
        QVERIFY(chooseTransport(desktop(
            roster({member(QStringLiteral("@legacy:example.org"), true)})))
                    .isMesh());
    }

    // The mirror case, and the one that cannot be papered over: Android
    // has no fall-forward. Refuse with a reason rather than join into
    // silent half-audio.
    void androidJoiningALiveKitCallIsRefusedWithAReason() {
        const TransportDecision d = chooseTransport(android(
            roster({member(QStringLiteral("@desktop:example.org"), true,
                           QLatin1String(kTransportLiveKit))})));
        QVERIFY(d.refused());
        QVERIFY(!d.isMesh());
        QVERIFY2(!d.refusalReason.isEmpty(),
                 "A refusal with no explanation is indistinguishable from an "
                 "outage. The user needs to know the channel is mesh-only.");
    }

    // A desktop client whose server has no [voice.livekit] block is in
    // exactly Android's position: mesh-only. The 404 from the token
    // endpoint IS the capability probe.
    void desktopWithoutAServerTokenIsMeshOnly() {
        TransportInputs in = desktop();
        in.serverOfferedLiveKitToken = false;
        QVERIFY(chooseTransport(in).isMesh());

        in.voiceMembers = roster({member(QStringLiteral("@d:example.org"), true,
                                         QLatin1String(kTransportLiveKit))});
        QVERIFY2(chooseTransport(in).refused(),
                 "No token means we cannot join an SFU channel, even though "
                 "this build has the SDK.");
    }

    // Both signals are required. Neither alone enables LiveKit.
    void liveKitRequiresBothClientSupportAndAServerToken() {
        TransportInputs in;
        in.localUserId = QStringLiteral("@me:example.org");

        in.clientSupportsLiveKit = false;
        in.serverOfferedLiveKitToken = false;
        QVERIFY(chooseTransport(in).isMesh());

        in.clientSupportsLiveKit = true;
        in.serverOfferedLiveKitToken = false;
        QVERIFY(chooseTransport(in).isMesh());

        in.clientSupportsLiveKit = false;
        in.serverOfferedLiveKitToken = true;
        QVERIFY(chooseTransport(in).isMesh());

        in.clientSupportsLiveKit = true;
        in.serverOfferedLiveKitToken = true;
        QVERIFY(chooseTransport(in).isLiveKit());
    }

    // A channel that has somehow ended up mixed already (a bug, or a
    // client that ignored a refusal) must not be made worse. Mesh
    // members win, because they are the ones who cannot adapt.
    void anAlreadyMixedChannelResolvesTowardsMesh() {
        const TransportDecision d = chooseTransport(desktop(
            roster({member(QStringLiteral("@a:example.org"), true,
                           QLatin1String(kTransportMesh)),
                    member(QStringLiteral("@b:example.org"), true,
                           QLatin1String(kTransportLiveKit))})));
        QVERIFY(d.isMesh());
    }

    // Reaped members do not pin the transport: an Android user who
    // crashed out of a channel must not lock it to mesh forever.
    void aReapedMeshMemberDoesNotPinTheChannel() {
        QVERIFY(chooseTransport(desktop(
            roster({member(QStringLiteral("@android:example.org"), false,
                           QLatin1String(kTransportMesh))})))
                    .isLiveKit());
    }

    // The wire strings are API: they appear in m.call.member state
    // events and in server-side policy. Renaming one silently splits a
    // channel in half.
    void wireStringsAreStable() {
        QCOMPARE(QLatin1String(kTransportMesh), QLatin1String("mesh"));
        QCOMPARE(QLatin1String(kTransportLiveKit), QLatin1String("livekit"));
    }

    // Totality: no input combination falls through without a decision,
    // and a refusal always carries a reason while a success never does.
    void decisionIsTotalAndWellFormed() {
        const QJsonArray rosters[] = {
            {},
            roster({member(QStringLiteral("@a:e.org"), true)}),
            roster({member(QStringLiteral("@a:e.org"), true,
                           QLatin1String(kTransportMesh))}),
            roster({member(QStringLiteral("@a:e.org"), true,
                           QLatin1String(kTransportLiveKit))}),
            roster({member(QStringLiteral("@a:e.org"), false,
                           QLatin1String(kTransportLiveKit))}),
        };
        for (bool client : {false, true}) {
            for (bool token : {false, true}) {
                for (const QJsonArray& r : rosters) {
                    TransportInputs in;
                    in.clientSupportsLiveKit = client;
                    in.serverOfferedLiveKitToken = token;
                    in.voiceMembers = r;
                    in.localUserId = QStringLiteral("@me:e.org");
                    const TransportDecision d = chooseTransport(in);
                    QCOMPARE(d.refused(), !d.refusalReason.isEmpty());
                    QVERIFY(d.isMesh() || d.isLiveKit() || d.refused());
                    // Exactly one of the three.
                    QCOMPARE(int(d.isMesh()) + int(d.isLiveKit()) + int(d.refused()), 1);
                    // A client with no SDK can never be sent to LiveKit.
                    if (!client) {
                        QVERIFY(!d.isLiveKit());
                    }
                }
            }
        }
    }
};

QTEST_MAIN(TestVoiceTransport)
#include "test_voice_transport.moc"
