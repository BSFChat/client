// "Hide my IP address": the decision table, exhaustively.
//
// The rule being pinned is in src/voice/IpPrivacy.h. The short version: voice
// and video are peer-to-peer, so a call tells the other side where you are.
// Relay-only ICE stops that by gathering relay candidates ONLY — no host
// candidate carrying the LAN address, no server-reflexive one carrying the
// public IP, so neither is ever put on the wire.
//
// Every wrong answer in this table is silent, which is why it gets a test each:
//
//   * All when Relay was asked for → the address goes out, with nothing said
//     and the switch still reading "on".
//   * Relay when there is no TURN server → ICE gathers nothing at all, every
//     peer sits in Connecting until the 30 s watchdog reaps it, and the user is
//     told nothing about that either.
//   * A fall back from Relay to All on a missing TURN server → the worst of the
//     three, because it looks like success and publishes exactly the address
//     the user asked to hide.
//
// Pure logic. No audio device, no network, no TURN server, no libdatachannel.

#include <QtTest>

#include "voice/IpPrivacy.h"
#include "voice/VoiceStartPolicy.h"
#include "voice/video/ReceiverReportEstimator.h"

using namespace voice;

namespace {

IcePolicyInputs inputs(RelayMode mode, bool share, bool allowP2P, bool turn) {
    IcePolicyInputs in;
    in.userMode = mode;
    in.shareHidesIp = share;
    in.serverAllowsP2P = allowP2P;
    in.hasTurn = turn;
    return in;
}

} // namespace

class TestIpPrivacy : public QObject {
    Q_OBJECT

private slots:
    // ---- The setting itself ----------------------------------------

    void theWireValuesAreStable() {
        QCOMPARE(relayModeToString(RelayMode::Auto), QStringLiteral("auto"));
        QCOMPARE(relayModeToString(RelayMode::RelayOnly), QStringLiteral("relayOnly"));
        QCOMPARE(relayModeFromString(QStringLiteral("relayOnly")), RelayMode::RelayOnly);
        QCOMPARE(relayModeFromString(QStringLiteral("auto")), RelayMode::Auto);
    }

    // Anything unrecognised reads as Auto, and the direction matters: Auto is
    // the value that cannot refuse a join. A settings file written by a newer
    // build, hand-edited, or simply empty must leave the client connectable.
    void anUnknownStoredValueFallsBackToAuto() {
        for (const QString& junk : {QStringLiteral(""), QStringLiteral("relay"),
                                    QStringLiteral("RELAYONLY"), QStringLiteral("yes"),
                                    QStringLiteral("relayOnly ")}) {
            QCOMPARE(relayModeFromString(junk), RelayMode::Auto);
        }
    }

    // ---- The policy decision, every row ----------------------------

    // The default. Nothing asks for relay, so the fastest route is used and
    // the session is not refused whether or not a TURN server exists.
    void autoOnAP2pServerIsDirect() {
        for (bool turn : {false, true}) {
            const auto d = decideIcePolicy(inputs(RelayMode::Auto, false, true, turn));
            QVERIFY(!d.relayOnly());
            QVERIFY(!d.refused());
            QCOMPARE(d.source, RelaySource::None);
            QVERIFY(!d.localChoice());
        }
    }

    // The pre-existing server-wide behaviour, unchanged. This is the row that
    // must not have moved: it is what production runs today.
    void aServerThatForbidsP2pStillForcesRelay() {
        const auto ok = decideIcePolicy(inputs(RelayMode::Auto, false, false, true));
        QVERIFY(ok.relayOnly());
        QVERIFY(!ok.refused());
        QCOMPARE(ok.source, RelaySource::Server);
        // Not a personal privacy choice, and must not be presented as one.
        QVERIFY(!ok.localChoice());

        const auto bad = decideIcePolicy(inputs(RelayMode::Auto, false, false, false));
        QVERIFY(bad.refused());
        QCOMPARE(bad.refusal, StartRefusal::RelayOnlyNoTurn);
    }

    // The new one, and the point of the whole feature: unilateral. The server
    // allows P2P and every peer may be using it; this client still relays.
    void theUserSettingForcesRelayEvenWhereP2pIsAllowed() {
        const auto d = decideIcePolicy(inputs(RelayMode::RelayOnly, false, true, true));
        QVERIFY(d.relayOnly());
        QVERIFY(!d.refused());
        QCOMPARE(d.source, RelaySource::UserSetting);
        QVERIFY(d.localChoice());
    }

    void thePerShareOptionForcesRelayEvenWhereP2pIsAllowed() {
        const auto d = decideIcePolicy(inputs(RelayMode::Auto, true, true, true));
        QVERIFY(d.relayOnly());
        QVERIFY(!d.refused());
        QCOMPARE(d.source, RelaySource::ShareOption);
        QVERIFY(d.localChoice());
    }

    // THE critical row. Relay-only with nothing to relay through must refuse,
    // and must never come back as All: falling back would publish the exact
    // address the setting exists to hide, and would do it silently.
    void relayOnlyWithNoTurnRefusesAndNeverFallsBackToDirect() {
        for (auto in : {inputs(RelayMode::RelayOnly, false, true, false),
                        inputs(RelayMode::Auto, true, true, false),
                        inputs(RelayMode::RelayOnly, true, false, false)}) {
            const auto d = decideIcePolicy(in);
            QVERIFY2(d.refused(), "a relay-only session with no TURN was allowed");
            QCOMPARE(d.refusal, StartRefusal::RelayOnlyNoTurn);
            QVERIFY2(d.relayOnly(),
                     "the refused decision reports a direct policy — a caller that "
                     "ignored the refusal would connect and publish the address");
        }
    }

    // The full 2x2x2x2, asserted as one invariant rather than sixteen cases:
    // relay is chosen if and only if something asked for it, and a refusal
    // happens if and only if relay was chosen with no TURN.
    void theWholeTableAgreesWithTheRule() {
        for (auto mode : {RelayMode::Auto, RelayMode::RelayOnly}) {
            for (bool share : {false, true}) {
                for (bool p2p : {false, true}) {
                    for (bool turn : {false, true}) {
                        const auto d = decideIcePolicy(inputs(mode, share, p2p, turn));
                        const bool wanted =
                            mode == RelayMode::RelayOnly || share || !p2p;
                        QCOMPARE(d.relayOnly(), wanted);
                        QCOMPARE(d.refused(), wanted && !turn);
                        QCOMPARE(d.source != RelaySource::None, wanted);
                        QCOMPARE(d.localChoice(),
                                 mode == RelayMode::RelayOnly || share);
                    }
                }
            }
        }
    }

    // Which source is NAMED when several apply. Most specific first, because
    // the refusal text sends the reader somewhere and the per-share option is
    // the thing they just clicked.
    void theMostSpecificSourceIsTheOneNamed() {
        QCOMPARE(decideIcePolicy(inputs(RelayMode::RelayOnly, true, false, true)).source,
                 RelaySource::ShareOption);
        QCOMPARE(decideIcePolicy(inputs(RelayMode::RelayOnly, false, false, true)).source,
                 RelaySource::UserSetting);
        QCOMPARE(decideIcePolicy(inputs(RelayMode::Auto, false, false, true)).source,
                 RelaySource::Server);
    }

    // ---- The per-share transition ----------------------------------
    //
    // Modelled as "what the effective policy is before and after the flag
    // moves", which is exactly the comparison VoiceEngine makes to decide
    // whether every peer connection has to be rebuilt. Rebuilding costs a
    // reconnect, so doing it when nothing changed is a real cost, and NOT
    // doing it when something did is a policy that is not enforced.

    void aShareStartingUnderAutoFlipsThePolicyAndNeedsARebuild() {
        const auto before = decideIcePolicy(inputs(RelayMode::Auto, false, true, true));
        const auto after = decideIcePolicy(inputs(RelayMode::Auto, true, true, true));
        QVERIFY(!before.relayOnly());
        QVERIFY(after.relayOnly());
        QVERIFY2(before.relayOnly() != after.relayOnly(),
                 "the engine would not rebuild, so the share would go out over "
                 "connections that still publish the address");
    }

    void aShareEndingReturnsToTheUsersOwnSetting() {
        // Auto: back to direct.
        QVERIFY(!decideIcePolicy(inputs(RelayMode::Auto, false, true, true)).relayOnly());
        // RelayOnly: stays relayed. Ending a share must not undo the standing
        // setting — that would turn a share into a way to lose the protection
        // you had before it started.
        QVERIFY(decideIcePolicy(inputs(RelayMode::RelayOnly, false, true, true)).relayOnly());
    }

    // No rebuild when the policy is already what the share is asking for.
    // Every rebuild is a visible reconnect for everyone in the call, so an
    // unnecessary one is not free.
    void aShareChangesNothingWhenTheUserIsAlreadyRelayOnly() {
        const auto before = decideIcePolicy(inputs(RelayMode::RelayOnly, false, true, true));
        const auto after = decideIcePolicy(inputs(RelayMode::RelayOnly, true, true, true));
        QCOMPARE(before.relayOnly(), after.relayOnly());
    }

    void aShareChangesNothingOnARelayOnlyServer() {
        const auto before = decideIcePolicy(inputs(RelayMode::Auto, false, false, true));
        const auto after = decideIcePolicy(inputs(RelayMode::Auto, true, false, true));
        QCOMPARE(before.relayOnly(), after.relayOnly());
    }

    // ---- Rate control across a policy switch -----------------------
    //
    // A policy switch rebuilds every peer connection, and a rebuilt connection
    // starts its RTP sequence numbers and byte counters from scratch. The
    // loss-based controller must see that as a NEW connection, not as a
    // catastrophic loss event on the old one — otherwise flipping the privacy
    // setting collapses the bitrate on a link that is merely different, and
    // the owner's "video quality is excellent" becomes "video quality fell
    // over when I turned that on".
    //
    // VoiceEngine::removePeer() erases the peer's estimator snapshot, so the
    // replacement connection meets a fresh estimator. These assert what
    // "fresh" then does — the relay path is not special-cased anywhere, it
    // simply adapts, which is the whole design.

    void aFreshEstimatorSeedsAndDoesNotGradeItsFirstReports() {
        ReceiverReportEstimator est;
        // Report 1 after a rebuild: counters back at zero. Seeds, grades
        // nothing.
        auto r1 = est.update(0, 0, 0, true, 1000);
        QVERIFY(!r1.governs());
        QCOMPARE(r1.expected, quint64(0));

        // Report 2: the first difference after a seed, discarded by the
        // warm-up rule. Still no verdict.
        auto r2 = est.update(50000, 400, 0, true, 2000);
        QVERIFY(!r2.governs());

        // Report 3 is the first real one, and on a clean relay it reads clean.
        auto r3 = est.update(100000, 800, 0, true, 3000);
        QCOMPARE(r3.expected, quint64(400));
        QCOMPARE(r3.lost, quint64(0));
        QVERIFY(r3.goodputKbps > 0.0);
    }

    // The failure this guards directly: an estimator that carried the OLD
    // connection's counters across the rebuild. Counters going backwards must
    // re-seed silently, not be read as "we lost everything".
    void countersGoingBackwardsReseedRatherThanReportTotalLoss() {
        ReceiverReportEstimator est;
        est.update(0, 0, 0, true, 1000);
        est.update(50000, 400, 0, true, 2000);
        auto graded = est.update(100000, 800, 0, true, 3000);
        QVERIFY(graded.governs());

        // The switch happens: new connection, counters restart at zero.
        auto after = est.update(0, 0, 0, true, 4000);
        QVERIFY2(!after.governs(),
                 "a rebuilt connection's reset counters were graded as loss — the "
                 "bitrate would collapse every time the privacy policy changes");
        QCOMPARE(after.lost, quint64(0));
    }

    // ---- The per-peer route ----------------------------------------

    void thePathVocabularyIsStable() {
        // Read verbatim by QML alongside peerState's, so it is as fixed as
        // that one is.
        QCOMPARE(peerPathName(PeerPath::Direct), QStringLiteral("direct"));
        QCOMPARE(peerPathName(PeerPath::Relayed), QStringLiteral("relayed"));
        // Unknown is the empty string, and the overlay renders nothing for it
        // — not "direct", which would show a route that has not been chosen.
        QVERIFY(peerPathName(PeerPath::Unknown).isEmpty());
    }
};

QTEST_MAIN(TestIpPrivacy)
#include "test_ip_privacy.moc"
