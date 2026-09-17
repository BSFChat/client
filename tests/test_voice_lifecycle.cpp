// Voice join/leave lifecycle — the state machine that replaced the
// `m_voiceTurnFetchPending` bool in ServerConnection.
//
// Everything here runs without a server, without libdatachannel, without
// an audio device and without a GUI: VoiceSession's outbound requests are
// signals (recorded by FakeMatrixClient below) and its transport is four
// std::function hooks (recorded by FakeEngine). That is the seam Phase 0
// of the plan asks for — before it, the join path had zero test coverage
// of any kind, which is how V-C1 (≈35 s of silence on every second join)
// survived four months of "verified by review".
//
// Each test names the audit item it pins down.

#include "net/VoiceQuit.h"
#include "net/VoiceSession.h"
#include "voice/CallEventOutbox.h"
#include "voice/VoiceRosterReconcile.h"
#include "voice/VoiceStartPolicy.h"
#include "voice/VoiceTransportSelector.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

namespace {

QJsonArray membersOf(const QStringList& ids)
{
    QJsonArray out;
    for (const QString& id : ids) {
        QJsonObject row;
        row["user_id"] = id;
        row["active"] = true;
        out.append(row);
    }
    return out;
}

QJsonObject turnConfig(int ttlSeconds = 3600)
{
    QJsonObject cfg;
    cfg["uris"] = QJsonArray{QStringLiteral("turn:turn.example:3478")};
    cfg["username"] = QStringLiteral("u");
    cfg["password"] = QStringLiteral("p");
    cfg["ttl"] = ttlSeconds;
    cfg["allow_p2p"] = false;
    return cfg;
}

CallSignal invite(const QString& from, const QString& room, qint64 ts)
{
    CallSignal s;
    s.type = QStringLiteral("m.call.invite");
    s.sender = from;
    s.roomId = room;
    s.originServerTs = ts;
    s.payload = QByteArray("{\"call_id\":\"c1\"}");
    return s;
}

// ---------------------------------------------------------------------
// The seam: a stand-in for MatrixClient's voice REST surface.
//
// It records every request VoiceSession makes and lets a test answer
// them in any order, including "never". Replies are the same five inputs
// the real MatrixClient produces: voiceJoined, voiceLeft, voiceError (now
// split per endpoint), turnConfigResult and its new turnConfigError.
// ---------------------------------------------------------------------
class FakeMatrixClient : public QObject {
    Q_OBJECT
public:
    explicit FakeMatrixClient(VoiceSession* session, QObject* parent = nullptr)
        : QObject(parent), m_session(session)
    {
        connect(session, &VoiceSession::joinRequested, this,
                [this](const QString& room) {
            requests.append(QStringLiteral("join:") + room);
            joins.append(room);
        });
        connect(session, &VoiceSession::leaveRequested, this,
                [this](const QString& room, const QString& sessionId) {
            requests.append(QStringLiteral("leave:") + room);
            leaves.append(room);
            leaveTokens.append(sessionId);
        });
        connect(session, &VoiceSession::turnConfigRequested, this, [this]() {
            requests.append(QStringLiteral("turn"));
            ++turnFetches;
        });
        connect(session, &VoiceSession::stateUpdateRequested, this,
                [this](const QString& room, bool muted, bool deafened,
                       const QString& sessionId) {
            stateTokens.append(sessionId);
            requests.append(QStringLiteral("state:%1:%2:%3")
                                .arg(room)
                                .arg(muted ? 1 : 0)
                                .arg(deafened ? 1 : 0));
            stateUpdates.append(qMakePair(muted, deafened));
        });
        connect(session, &VoiceSession::mediaStateUpdateRequested, this,
                [this](const QString& room, bool screen, bool camera,
                       const QString& sessionId) {
            Q_UNUSED(sessionId);
            requests.append(QStringLiteral("media:%1:%2:%3")
                                .arg(room).arg(screen ? 1 : 0).arg(camera ? 1 : 0));
        });
    }

    // --- replies, driven by the test -------------------------------
    // Every join reply mints a new session token, exactly as the server
    // does — the client must never echo a token from a past membership.
    void replyJoinOk(const QString& room, const QStringList& members = {})
    {
        lastIssuedToken = QStringLiteral("sess-%1").arg(++m_tokenCounter);
        m_session->onJoinSucceeded(room, membersOf(members), lastIssuedToken);
    }
    void replyJoinError(const QString& room, const QString& err)
    { m_session->onJoinFailed(room, err); }
    void replyLeaveOk(const QString& room) { m_session->onLeaveSucceeded(room); }
    void replyLeaveError(const QString& room, const QString& err)
    { m_session->onLeaveFailed(room, err); }
    void replyTurn(const QJsonObject& cfg) { m_session->onTurnConfig(cfg); }
    void replyTurnError(const QString& err) { m_session->onTurnConfigFailed(err); }
    void replyStateOk(const QString& room)
    { m_session->onStateUpdateSucceeded(room); }
    void replyStateError(const QString& room, const QString& err)
    { m_session->onStateUpdateFailed(room, err); }

    QStringList requests;
    QStringList joins;
    QStringList leaves;
    QStringList leaveTokens;
    QStringList stateTokens;
    QString lastIssuedToken;
    QList<QPair<bool, bool>> stateUpdates;
    int turnFetches = 0;

private:
    VoiceSession* m_session;
    int m_tokenCounter = 0;
};

// The transport side of the seam.
class FakeEngine {
public:
    void install(VoiceSession* session)
    {
        session->setEngineStarter([this](const QString& room,
                                         const QJsonArray& members,
                                         const QJsonObject& cfg) {
            lastMembers = members;
            lastConfig = cfg;
            if (!startSucceeds) return false;
            running = true;
            room_ = room;
            ++starts;
            return true;
        });
        session->setEngineStopper([this]() {
            if (running) ++stops;
            running = false;
        });
        session->setSignalSink([this](const CallSignal& s) {
            delivered.append(s);
        });
        session->setTurnUpdater([this](const QJsonObject& cfg) {
            turnUpdates.append(cfg);
        });
        session->setMicGate([this](bool gate) { gates.append(gate); });
        session->setDeafenGate([this](bool d) { deafenGates.append(d); });
    }

    bool startSucceeds = true;
    bool running = false;
    int starts = 0;
    int stops = 0;
    QString room_;
    QJsonArray lastMembers;
    QJsonObject lastConfig;
    QVector<CallSignal> delivered;
    QVector<QJsonObject> turnUpdates;
    QVector<bool> gates;
    QVector<bool> deafenGates;
};

// Drives a session all the way to Active, leaving the fakes primed.
void joinTo(VoiceSession& s, FakeMatrixClient& net, const QString& room,
            const QStringList& members = {})
{
    s.requestJoin(room);
    net.replyJoinOk(room, members);
    net.replyTurn(turnConfig());
}

} // namespace

class TestVoiceLifecycle : public QObject {
    Q_OBJECT
private slots:

    // ---- the happy path, and that the states are actually traversed --
    void joinReachesActiveThroughEveryState();

    // ---- V-C1 -------------------------------------------------------
    void inviteDuringJoinIsBufferedNotLost();
    void inviteBeforeJoinReplyIsBufferedNotLost();
    void bufferIsBoundedAndDroppedOnUnwind();

    // ---- V-H1 -------------------------------------------------------
    void joinWaitsForAnInFlightLeave();
    void channelSwitchSendsLeaveBeforeJoin();

    // ---- V-M4 -------------------------------------------------------
    void unrelatedErrorDuringTurnWindowDoesNotUnwind();
    void turnFailureUnwindsTheJoin();
    void engineStartFailureUnwindsTheJoin();

    // ---- V-H2 -------------------------------------------------------
    void selfRetirementRejoinsOnce();
    void selfRetirementTearsDownWhenRejoinFails();

    // ---- session tokens (server workstream E) ------------------------
    void requestsCarryTheCurrentSessionToken();
    void retractionOfASupersededSessionIsIgnored();
    void supersededStatePutTriggersRecovery();

    // ---- V-M1 -------------------------------------------------------
    void staleInviteIsIgnored();

    // ---- V-M3 -------------------------------------------------------
    void turnIsRefetchedAtHalfLife();

    // ---- V-M7 -------------------------------------------------------
    void pttAnnouncesTheEffectiveGate();
    void failedStatePutRollsBackLocalMute();
    void muteSurvivesAChannelSwitch();

    // ---- V-M5 -------------------------------------------------------
    void rosterReconcilerAddsAndPrunes();

    // ---- V-H4 -------------------------------------------------------
    void startPolicyRefusesWithoutAudioOrRelay();
    void deniedMicrophoneRefusesTheJoin();

    // ---- V-H3 -------------------------------------------------------
    void settledFiresOnceEveryLeaveIsAnswered();
    void quitWaitsForLeavesButIsBounded();

    // ---- V-M2 -------------------------------------------------------
    void outboxRetriesWithBackoffThenGivesUp();
    void outboxKeepsTheCandidateBatchUntilItIsAccepted();
    void leavingFlushesTheHangupsStopItselfQueued();
    void aFailedPeerKeepsSayingFailedInsteadOfNew();
    void joiningWhileDeafenedDeafensTheNewEngine();

    // ---- V-L4 -------------------------------------------------------
    void transportSelectorRefusesMixedRoster();
};

void TestVoiceLifecycle::joinReachesActiveThroughEveryState()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    QCOMPARE(s.state(), VoiceSession::State::Idle);
    s.requestJoin("!room:x");
    QCOMPARE(s.state(), VoiceSession::State::Joining);
    QCOMPARE(net.joins, QStringList{"!room:x"});
    QCOMPARE(net.turnFetches, 0);   // nothing runs ahead of the join reply

    net.replyJoinOk("!room:x", {"@me:x", "@other:x"});
    QCOMPARE(s.state(), VoiceSession::State::FetchingTurn);
    QCOMPARE(net.turnFetches, 1);
    QVERIFY(!engine.running);

    net.replyTurn(turnConfig());
    QCOMPARE(s.state(), VoiceSession::State::Active);
    QVERIFY(s.isActive());
    QCOMPARE(engine.starts, 1);
    QCOMPARE(engine.lastMembers.size(), 2);
    QVERIFY(!s.busy());

    s.requestLeave();
    QCOMPARE(s.state(), VoiceSession::State::Leaving);
    QCOMPARE(engine.stops, 1);      // local teardown precedes the reply
    QCOMPARE(net.leaves, QStringList{"!room:x"});
    net.replyLeaveOk("!room:x");
    QCOMPARE(s.state(), VoiceSession::State::Idle);
    QVERIFY(s.roomId().isEmpty());
}

void TestVoiceLifecycle::inviteDuringJoinIsBufferedNotLost()
{
    // V-C1. The sitting member sends its invite the moment it sees our
    // m.call.member, which is during our TURN fetch. The old code
    // discarded it — the offerer then sat in Connecting until its 30 s
    // watchdog re-offered.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    s.requestJoin("!room:x");
    net.replyJoinOk("!room:x");
    QCOMPARE(s.state(), VoiceSession::State::FetchingTurn);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    s.routeInboundSignal(invite("@aaa:x", "!room:x", now));
    QCOMPARE(s.bufferedSignalCount(), 1);
    QCOMPARE(engine.delivered.size(), 0);

    net.replyTurn(turnConfig());
    QCOMPARE(s.bufferedSignalCount(), 0);
    QCOMPARE(engine.delivered.size(), 1);
    QCOMPARE(engine.delivered.first().sender, QStringLiteral("@aaa:x"));
    // And the replay happens AFTER the engine exists, not before.
    QVERIFY(engine.running);

    // Once active, signalling flows straight through.
    s.routeInboundSignal(invite("@bbb:x", "!room:x", now));
    QCOMPARE(engine.delivered.size(), 2);
    QCOMPARE(s.bufferedSignalCount(), 0);
}

void TestVoiceLifecycle::inviteBeforeJoinReplyIsBufferedNotLost()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    s.requestJoin("!room:x");
    s.routeInboundSignal(invite("@aaa:x", "!room:x",
                                QDateTime::currentMSecsSinceEpoch()));
    QCOMPARE(s.bufferedSignalCount(), 1);
    net.replyJoinOk("!room:x");
    net.replyTurn(turnConfig());
    QCOMPARE(engine.delivered.size(), 1);

    // Signalling for a room we are not joining is never buffered.
    s.routeInboundSignal(invite("@aaa:x", "!elsewhere:x",
                                QDateTime::currentMSecsSinceEpoch()));
    QCOMPARE(engine.delivered.size(), 1);
    // Nor is our own echo.
    s.routeInboundSignal(invite("@me:x", "!room:x",
                                QDateTime::currentMSecsSinceEpoch()));
    QCOMPARE(engine.delivered.size(), 1);
}

void TestVoiceLifecycle::bufferIsBoundedAndDroppedOnUnwind()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    s.requestJoin("!room:x");
    net.replyJoinOk("!room:x");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < VoiceSession::kMaxBufferedSignals + 50; ++i)
        s.routeInboundSignal(invite("@aaa:x", "!room:x", now));
    QCOMPARE(s.bufferedSignalCount(), VoiceSession::kMaxBufferedSignals);

    net.replyTurnError("boom");
    QCOMPARE(s.bufferedSignalCount(), 0);
    QCOMPARE(engine.delivered.size(), 0);
}

void TestVoiceLifecycle::joinWaitsForAnInFlightLeave()
{
    // V-H1: leave then immediate rejoin. The two used to go out on
    // parallel connections; if the server processed the join first the
    // user ended up active=false but believing they were in the call.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!room:x");
    s.requestLeave();
    QCOMPARE(s.state(), VoiceSession::State::Leaving);

    net.requests.clear();
    s.requestJoin("!room:x");           // rejoin the SAME room at once
    QVERIFY(net.requests.isEmpty());    // nothing on the wire yet
    QCOMPARE(s.queuedRequestCount(), 1);

    net.replyLeaveOk("!room:x");        // leave answered → join goes out
    QCOMPARE(net.requests, QStringList{"join:!room:x"});
    QCOMPARE(s.state(), VoiceSession::State::Joining);
}

void TestVoiceLifecycle::channelSwitchSendsLeaveBeforeJoin()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!a:x");
    net.requests.clear();

    s.requestJoin("!b:x");
    // Local teardown is immediate; the join is NOT.
    QCOMPARE(engine.stops, 1);
    QCOMPARE(net.requests, QStringList{"leave:!a:x"});

    net.replyLeaveOk("!a:x");
    QCOMPARE(net.requests, (QStringList{"leave:!a:x", "join:!b:x"}));

    net.replyJoinOk("!b:x");
    net.replyTurn(turnConfig());
    QCOMPARE(s.roomId(), QStringLiteral("!b:x"));
    QCOMPARE(engine.starts, 2);

    // Asking for the room we are already in is a no-op, not a rejoin.
    net.requests.clear();
    s.requestJoin("!b:x");
    QVERIFY(net.requests.isEmpty());
}

void TestVoiceLifecycle::unrelatedErrorDuringTurnWindowDoesNotUnwind()
{
    // V-M4. On a channel switch the PREVIOUS room's leave could fail
    // while the new room's TURN fetch was in flight; the one generic
    // voiceError signal then unwound a join that was perfectly healthy.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!a:x");
    s.requestJoin("!b:x");
    net.replyLeaveOk("!a:x");
    net.replyJoinOk("!b:x");
    QCOMPARE(s.state(), VoiceSession::State::FetchingTurn);

    // A late failure attributed to the OLD room, mid TURN window.
    net.replyLeaveError("!a:x", "leave failed");
    QCOMPARE(s.state(), VoiceSession::State::FetchingTurn);

    // A failed member poll in the same window is not a join failure either.
    net.replyStateError("!b:x", "poll failed");
    QCOMPARE(s.state(), VoiceSession::State::FetchingTurn);

    net.replyTurn(turnConfig());
    QCOMPARE(s.state(), VoiceSession::State::Active);
    QCOMPARE(engine.starts, 2);
}

void TestVoiceLifecycle::turnFailureUnwindsTheJoin()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);
    QSignalSpy errors(&s, &VoiceSession::errorOccurred);

    s.requestJoin("!room:x");
    net.replyJoinOk("!room:x");
    net.replyTurnError("turn is down");

    // The membership must be dropped server side, or the 5 s member poll
    // keeps the heartbeat alive and the ghost reaper never fires — so the
    // unwind is not finished until that leave has been answered.
    QCOMPARE(s.state(), VoiceSession::State::Leaving);
    QCOMPARE(net.leaves, QStringList{"!room:x"});
    QCOMPARE(errors.size(), 1);
    QCOMPARE(engine.starts, 0);
    QVERIFY(!s.isActive());

    net.replyLeaveOk("!room:x");
    QCOMPARE(s.state(), VoiceSession::State::Idle);
    QVERIFY(s.roomId().isEmpty());
}

void TestVoiceLifecycle::engineStartFailureUnwindsTheJoin()
{
    // V-H4's caller half: start() returning false (no audio device,
    // relay-only with no TURN) must unwind the join, not leave a ghost.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.startSucceeds = false;
    engine.install(&s);

    s.requestJoin("!room:x");
    net.replyJoinOk("!room:x");
    net.replyTurn(turnConfig());

    QCOMPARE(s.state(), VoiceSession::State::Leaving);
    QCOMPARE(net.leaves, QStringList{"!room:x"});
    QVERIFY(!engine.running);
    QVERIFY(!s.isActive());

    // And a join issued straight afterwards still goes out AFTER that
    // unwind leave — the ordering guarantee has to survive the failure.
    net.requests.clear();
    s.requestJoin("!room:x");
    QCOMPARE(net.requests, QStringList{});   // still waiting on the leave
    net.replyLeaveOk("!room:x");
    QCOMPARE(net.requests, QStringList{"join:!room:x"});
}

void TestVoiceLifecycle::selfRetirementRejoinsOnce()
{
    // V-H2. The reaper (or a moderator, or the V-H1 race) sets our own
    // m.call.member active=false. The old client ignored it entirely and
    // stayed reaped forever: engine up, mic hot, unlisted.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!room:x");
    net.requests.clear();

    s.onSelfMembership("!room:x", false);
    QCOMPARE(net.requests, QStringList{"join:!room:x"});
    QVERIFY(s.isActive());           // engine keeps running meanwhile
    QCOMPARE(engine.stops, 0);

    net.replyJoinOk("!room:x", {"@me:x"});
    QVERIFY(s.isActive());
    QCOMPARE(engine.starts, 1);      // NOT restarted
    QCOMPARE(engine.stops, 0);

    // A second retirement after a healthy row gets its own attempt.
    s.onSelfMembership("!room:x", true);
    net.requests.clear();
    s.onSelfMembership("!room:x", false);
    QCOMPARE(net.requests, QStringList{"join:!room:x"});
}

void TestVoiceLifecycle::selfRetirementTearsDownWhenRejoinFails()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);
    QSignalSpy errors(&s, &VoiceSession::errorOccurred);

    joinTo(s, net, "!room:x");
    s.onSelfMembership("!room:x", false);
    net.replyJoinError("!room:x", "not in channel");

    QCOMPARE(s.state(), VoiceSession::State::Idle);
    QCOMPARE(engine.stops, 1);
    QCOMPARE(errors.size(), 1);

    // Retired twice in a row without the re-join ever failing: the
    // second retirement tears down rather than looping on re-joins.
    VoiceSession s2;
    s2.setLocalUserId("@me:x");
    FakeMatrixClient net2(&s2);
    FakeEngine engine2;
    engine2.install(&s2);
    joinTo(s2, net2, "!room:x");
    s2.onSelfMembership("!room:x", false);
    net2.replyJoinOk("!room:x");
    s2.onSelfMembership("!room:x", false);   // still retired
    QCOMPARE(s2.state(), VoiceSession::State::Idle);
    QCOMPARE(engine2.stops, 1);
}

void TestVoiceLifecycle::requestsCarryTheCurrentSessionToken()
{
    // The server refuses a leave or a state PUT whose token its stored row
    // has moved past. That is what makes leave-then-join robust even if the
    // two cross on the wire — but only if we echo the RIGHT token.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!a:x");
    const QString first = net.lastIssuedToken;
    QCOMPARE(s.sessionId(), first);

    s.toggleMute();
    QCOMPARE(net.stateTokens.last(), first);
    net.replyStateOk("!a:x");

    s.requestJoin("!b:x");
    QCOMPARE(net.leaveTokens.last(), first);   // leave names the room it leaves
    net.replyLeaveOk("!a:x");
    // The join itself carries no token — it is the request that mints one.
    net.replyJoinOk("!b:x");
    const QString second = net.lastIssuedToken;
    QVERIFY(second != first);
    net.replyTurn(turnConfig());
    QCOMPARE(s.sessionId(), second);

    // The carried-over mute is announced on the fresh row; let that PUT
    // finish so the leave is not simply queued behind it.
    QCOMPARE(net.stateTokens.last(), second);
    net.replyStateOk("!b:x");

    net.leaveTokens.clear();
    s.requestLeave();
    QCOMPARE(net.leaveTokens, QStringList{second});
    net.replyLeaveOk("!b:x");
    QVERIFY(s.sessionId().isEmpty());
}

void TestVoiceLifecycle::retractionOfASupersededSessionIsIgnored()
{
    // A join over an active row makes the server emit active=false naming
    // the OLD session, then active=true — an edge for peers to reset on.
    // Acting on that retraction would tear down the session it just
    // created.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!room:x");
    const QString current = net.lastIssuedToken;
    net.requests.clear();

    s.onSelfMembership("!room:x", false, QStringLiteral("sess-older"));
    QVERIFY(net.requests.isEmpty());        // no re-join, no teardown
    QVERIFY(s.isActive());
    QCOMPARE(engine.stops, 0);

    // The retraction of OUR session still triggers the V-H2 recovery.
    s.onSelfMembership("!room:x", false, current);
    QCOMPARE(net.requests, QStringList{"join:!room:x"});

    // A server that issues no token at all keeps the old behaviour.
    VoiceSession legacy;
    legacy.setLocalUserId("@me:x");
    FakeMatrixClient legacyNet(&legacy);
    FakeEngine legacyEngine;
    legacyEngine.install(&legacy);
    joinTo(legacy, legacyNet, "!room:x");
    legacyNet.requests.clear();
    legacy.onSelfMembership("!room:x", false, QString());
    QCOMPARE(legacyNet.requests, QStringList{"join:!room:x"});
}

void TestVoiceLifecycle::supersededStatePutTriggersRecovery()
{
    // 403 on voice/state means the row carries a newer token than ours.
    // A reaped row is never re-activated by a state PUT, so the only
    // recovery is re-POSTing the join.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!room:x");
    s.toggleMute();
    net.requests.clear();

    s.onStateSuperseded("!room:x");
    s.onStateUpdateFailed("!room:x", "Voice session superseded");
    QCOMPARE(net.joins.last(), QStringLiteral("!room:x"));
    QVERIFY(s.isActive());          // the engine is not torn down first

    // And if that re-join is refused, the session gives up cleanly.
    net.replyJoinError("!room:x", "not in channel");
    QCOMPARE(s.state(), VoiceSession::State::Idle);
    QCOMPARE(engine.stops, 1);
}

void TestVoiceLifecycle::staleInviteIsIgnored()
{
    // V-M1. A full-sync fallback replays the tail of the room timeline,
    // which on a voice channel is old invites with dead call ids. Applying
    // one looks like "peer restarted" and tears down a healthy peer.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!room:x");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    s.routeInboundSignal(invite("@aaa:x", "!room:x", now - 3600'000));
    QCOMPARE(engine.delivered.size(), 0);

    // Inside the clock-skew allowance: kept, because origin_server_ts is
    // the server's clock and ours may differ.
    s.routeInboundSignal(invite("@aaa:x", "!room:x", now - 1000));
    QCOMPARE(engine.delivered.size(), 1);

    // Unstamped events are never dropped as stale.
    s.routeInboundSignal(invite("@aaa:x", "!room:x", 0));
    QCOMPARE(engine.delivered.size(), 2);

    // Staleness only applies to invites: an old candidate batch for a
    // live call is still worth having.
    CallSignal cands = invite("@aaa:x", "!room:x", now - 3600'000);
    cands.type = QStringLiteral("m.call.candidates");
    s.routeInboundSignal(cands);
    QCOMPARE(engine.delivered.size(), 3);
}

void TestVoiceLifecycle::turnIsRefetchedAtHalfLife()
{
    // V-M3. Credentials were fetched once per session, so the mesh
    // reconciler built peer connections with expired TURN creds after
    // `ttl` had passed.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    s.requestJoin("!room:x");
    net.replyJoinOk("!room:x");
    net.replyTurn(turnConfig(/*ttlSeconds=*/2));   // half-life = 1 s
    QCOMPARE(net.turnFetches, 1);

    QTRY_VERIFY_WITH_TIMEOUT(net.turnFetches == 2, 4000);
    QVERIFY(s.isActive());                          // a refresh is not a re-join

    QJsonObject fresh = turnConfig(2);
    fresh["password"] = QStringLiteral("rotated");
    net.replyTurn(fresh);
    QCOMPARE(engine.turnUpdates.size(), 1);
    QCOMPARE(engine.turnUpdates.first().value("password").toString(),
             QStringLiteral("rotated"));
    QCOMPARE(engine.starts, 1);                     // engine not rebuilt
    QVERIFY(s.isActive());

    // A failed refresh keeps the session alive on the old credentials.
    net.replyTurnError("turn hiccup");
    QVERIFY(s.isActive());
}

void TestVoiceLifecycle::pttAnnouncesTheEffectiveGate()
{
    // V-M7. In PTT mode the mic is shut unless the key is held, but the
    // roster was told muted=false — so everyone saw an unmuted user who
    // was never audible.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    s.setPttMode(true);
    joinTo(s, net, "!room:x");

    QVERIFY(s.effectiveMuted());
    QVERIFY(!s.muted());                 // not a manual mute
    QVERIFY(!engine.gates.isEmpty());
    QCOMPARE(engine.gates.last(), true); // engine gated on join
    QVERIFY(net.stateUpdates.contains(qMakePair(true, false)));
    net.replyStateOk("!room:x");

    net.stateUpdates.clear();
    s.setPttPressed(true);
    QVERIFY(!s.effectiveMuted());
    QCOMPARE(engine.gates.last(), false);
    QCOMPARE(net.stateUpdates, (QList<QPair<bool, bool>>{{false, false}}));
    net.replyStateOk("!room:x");

    net.stateUpdates.clear();
    s.setPttPressed(false);
    QCOMPARE(net.stateUpdates, (QList<QPair<bool, bool>>{{true, false}}));
}

void TestVoiceLifecycle::failedStatePutRollsBackLocalMute()
{
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);
    joinTo(s, net, "!room:x");

    QSignalSpy muteSpy(&s, &VoiceSession::mutedChanged);
    s.toggleMute();
    QVERIFY(s.muted());
    QCOMPARE(net.stateUpdates.last(), qMakePair(true, false));

    net.replyStateError("!room:x", "nope");
    QVERIFY(!s.muted());                  // rolled back to what the server has
    QCOMPARE(muteSpy.size(), 2);
    QCOMPARE(engine.gates.last(), false); // and the engine follows the rollback

    // An accepted toggle sticks.
    s.toggleMute();
    net.replyStateOk("!room:x");
    QVERIFY(s.muted());
    s.toggleDeafen();
    net.replyStateError("!room:x", "nope");
    QVERIFY(!s.deafened());
    QVERIFY(s.muted());                   // the accepted mute is untouched
}

void TestVoiceLifecycle::muteSurvivesAChannelSwitch()
{
    // V-M7's third leg: joining reset mute to false, so a muted user who
    // changed channel started transmitting.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    joinTo(s, net, "!a:x");
    s.toggleMute();
    net.replyStateOk("!a:x");
    QVERIFY(s.muted());

    s.requestJoin("!b:x");
    net.replyLeaveOk("!a:x");
    net.replyJoinOk("!b:x");
    net.stateUpdates.clear();
    net.replyTurn(turnConfig());

    QVERIFY(s.muted());
    QCOMPARE(engine.gates.last(), true);
    // …and the new room is told about it rather than silently differing.
    QCOMPARE(net.stateUpdates, (QList<QPair<bool, bool>>{{true, false}}));
}

void TestVoiceLifecycle::rosterReconcilerAddsAndPrunes()
{
    // V-M5. The reconciler only ever ADDED peers; one that crashed stayed
    // "connected" in the UI until ICE gave up.
    const QString me = QStringLiteral("@mid:x");
    const QJsonArray roster = membersOf({me, "@zzz:x", "@aaa:x"});

    auto r = voice::reconcileRoster(roster, {"@gone:x", "@zzz:x"}, me);
    QCOMPARE(r.toDrop, QStringList{"@gone:x"});
    QCOMPARE(r.toOffer, QStringList{});     // @zzz held, @aaa is theirs to offer

    r = voice::reconcileRoster(roster, {}, me);
    QCOMPARE(r.toOffer, QStringList{"@zzz:x"});   // only ids above ours
    QCOMPARE(r.toDrop, QStringList{});

    // An inactive row is an absent row.
    QJsonArray withReaped = membersOf({me});
    QJsonObject reaped;
    reaped["user_id"] = QStringLiteral("@zzz:x");
    reaped["active"] = false;
    withReaped.append(reaped);
    r = voice::reconcileRoster(withReaped, {"@zzz:x"}, me);
    QCOMPARE(r.toDrop, QStringList{"@zzz:x"});
    QCOMPARE(r.toOffer, QStringList{});

    // We are never our own peer.
    r = voice::reconcileRoster(membersOf({me}), {me}, me);
    QCOMPARE(r.toDrop, QStringList{});
}

void TestVoiceLifecycle::startPolicyRefusesWithoutAudioOrRelay()
{
    // V-H4. start() used to return true after AudioEngine::start had
    // already failed, so the user joined deaf and mute with a toast.
    using voice::StartRefusal;
    QCOMPARE(voice::evaluateStart(/*allowP2P=*/true, /*hasRelay=*/false,
                                  /*audioStarted=*/true), StartRefusal::None);
    QCOMPARE(voice::evaluateStart(false, false, true),
             StartRefusal::RelayOnlyNoTurn);
    QCOMPARE(voice::evaluateStart(true, true, false),
             StartRefusal::AudioUnavailable);
    // The unusable-transport check wins: there is no point reporting a
    // microphone problem for a call that could never connect.
    QCOMPARE(voice::evaluateStart(false, false, false),
             StartRefusal::RelayOnlyNoTurn);
    QVERIFY(!voice::refusalMessage(StartRefusal::AudioUnavailable).isEmpty());
    QVERIFY(voice::refusalMessage(StartRefusal::None).isEmpty());
}

void TestVoiceLifecycle::deniedMicrophoneRefusesTheJoin()
{
    // V-H4, second half. rc.7 logged "microphone permission is denied in
    // system settings" and then joined anyway: the user sat in the
    // channel with a mic capturing silence (peak |sample| = 30 on a live
    // RØDE NT-USB+), their member row kept alive by the poll, with
    // nothing in the UI to explain why nobody could hear them.
    //
    // The OS status cannot be faked in-process — there is no way to
    // answer a TCC prompt from a test — so the decision is a value and
    // it is the value that is pinned here.
    using voice::MicPermission;
    using voice::MicPermissionAction;
    QCOMPARE(voice::micPermissionAction(MicPermission::Denied),
             MicPermissionAction::Refuse);
    // Never asked: fire the prompt and carry on; a denial at the prompt
    // fails AudioEngine::start, which unwinds the join.
    QCOMPARE(voice::micPermissionAction(MicPermission::Undetermined),
             MicPermissionAction::RequestThenProceed);
    QCOMPARE(voice::micPermissionAction(MicPermission::Granted),
             MicPermissionAction::Proceed);
    // A platform (or Qt) with no permission concept must not be turned
    // into a platform where voice never starts.
    QCOMPARE(voice::micPermissionAction(MicPermission::Unsupported),
             MicPermissionAction::Proceed);

    // The refusal has to name the switch the user must flip: "denied" on
    // its own sends people to the audio-device settings, which are fine.
    const QString msg =
        voice::refusalMessage(voice::StartRefusal::MicrophoneDenied);
    QVERIFY(!msg.isEmpty());
    QVERIFY(msg.contains(QStringLiteral("Microphone")));
#if defined(Q_OS_MACOS)
    QVERIFY(msg.contains(QStringLiteral("System Settings")));
    QVERIFY(msg.contains(QStringLiteral("Privacy")));
#endif
    // Distinct from the no-device refusal: same symptom for the user,
    // completely different fix.
    QVERIFY(msg != voice::refusalMessage(voice::StartRefusal::AudioUnavailable));
}

void TestVoiceLifecycle::settledFiresOnceEveryLeaveIsAnswered()
{
    // V-H3's testable half: the quit path needs to know when the leave
    // has actually been answered rather than merely queued.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);
    joinTo(s, net, "!room:x");

    QSignalSpy settled(&s, &VoiceSession::settled);
    s.requestLeave();
    QCOMPARE(settled.size(), 0);      // queued is not sent, sent is not answered
    net.replyLeaveOk("!room:x");
    QCOMPARE(settled.size(), 1);

    // A failed leave still settles: the local side is already down and
    // the server row will be reaped.
    joinTo(s, net, "!room:x");
    s.requestLeave();
    net.replyLeaveError("!room:x", "boom");
    QCOMPARE(settled.size(), 2);

    // Nothing to leave settles immediately.
    VoiceSession idle;
    QSignalSpy idleSettled(&idle, &VoiceSession::settled);
    idle.requestLeave();
    QCOMPARE(idle.state(), VoiceSession::State::Idle);
    QCOMPARE(idleSettled.size(), 1);
}

void TestVoiceLifecycle::quitWaitsForLeavesButIsBounded()
{
    // V-H3. aboutToQuit used to queue the leave POST on QNAM and then let
    // the process exit before the socket was written, so nothing ever
    // left the machine and the roster row lingered 30-40 s.
    VoiceSession a;
    VoiceSession b;
    a.setLocalUserId("@me:x");
    b.setLocalUserId("@me:y");
    FakeMatrixClient netA(&a);
    FakeMatrixClient netB(&b);
    FakeEngine engineA;
    FakeEngine engineB;
    engineA.install(&a);
    engineB.install(&b);
    joinTo(a, netA, "!a:x");
    joinTo(b, netB, "!b:y");

    a.requestLeave();
    b.requestLeave();
    // Both replies land shortly after the wait begins, as they would from
    // a server that is up.
    QTimer::singleShot(30, [&]() { netA.replyLeaveOk("!a:x"); });
    QTimer::singleShot(60, [&]() { netB.replyLeaveOk("!b:y"); });

    QElapsedTimer clock;
    clock.start();
    QVERIFY(voice::waitForSessionsToSettle({&a, &b}, 2000));
    QVERIFY(clock.elapsed() < 1500);          // returned on the replies…
    QVERIFY(a.state() == VoiceSession::State::Idle);
    QVERIFY(b.state() == VoiceSession::State::Idle);

    // …and a server that never answers does not stop the user quitting.
    joinTo(a, netA, "!a:x");
    a.requestLeave();
    clock.restart();
    QVERIFY(!voice::waitForSessionsToSettle({&a}, 250));
    QVERIFY(clock.elapsed() >= 200);
    QVERIFY(clock.elapsed() < 2000);

    // Nothing to wait for returns immediately without entering a loop.
    VoiceSession idle;
    QVERIFY(voice::waitForSessionsToSettle({&idle}, 5000));
    QVERIFY(voice::waitForSessionsToSettle({}, 5000));
    QVERIFY(voice::waitForSessionsToSettle({nullptr}, 5000));
}

void TestVoiceLifecycle::outboxRetriesWithBackoffThenGivesUp()
{
    // V-M2. Outbound signalling was fire-and-forget, so one lost invite,
    // answer or hangup cost a full 30 s: the peer sat in Connecting until
    // the setup watchdog reaped it and the reconciler tried again.
    voice::CallEventOutbox outbox;
    const qint64 t0 = 1'000'000;

    const quint64 token = outbox.add("m.call.invite", QByteArray("{}"), t0);
    QCOMPARE(outbox.pendingCount(), 1);

    auto due = outbox.due(t0);
    QCOMPARE(due.size(), 1);
    QCOMPARE(due.first().token, token);
    QCOMPARE(due.first().attempt, 1);
    // In flight: a second tick must not send it again while we wait.
    QVERIFY(outbox.due(t0 + 5000).isEmpty());

    QVERIFY(outbox.onFailed(token, t0));       // will retry
    QVERIFY(outbox.due(t0 + 499).isEmpty());   // …after the backoff
    due = outbox.due(t0 + 500);
    QCOMPARE(due.size(), 1);
    QCOMPARE(due.first().attempt, 2);
    QCOMPARE(due.first().payload, QByteArray("{}"));

    // Backoff grows and is capped.
    QCOMPARE(voice::CallEventOutbox::backoffMs(1), qint64(500));
    QCOMPARE(voice::CallEventOutbox::backoffMs(2), qint64(1000));
    QCOMPARE(voice::CallEventOutbox::backoffMs(3), qint64(2000));
    QCOMPARE(voice::CallEventOutbox::backoffMs(9),
             qint64(voice::CallEventOutbox::kMaxBackoffMs));

    // Bounded: a peer that is genuinely gone must not queue forever —
    // the setup watchdog is the better answer.
    qint64 now = t0 + 500;
    for (int attempt = 2; attempt < voice::CallEventOutbox::kMaxAttempts;
         ++attempt) {
        QVERIFY(outbox.onFailed(token, now));
        now += voice::CallEventOutbox::backoffMs(attempt);
        QCOMPARE(outbox.due(now).size(), 1);
    }
    QVERIFY(!outbox.onFailed(token, now));     // gave up
    QVERIFY(outbox.isEmpty());

    // Success forgets it, and an unknown token is harmless.
    const quint64 second = outbox.add("m.call.answer", QByteArray("{}"), now);
    outbox.due(now);
    outbox.onSent(second);
    QVERIFY(outbox.isEmpty());
    QVERIFY(!outbox.onFailed(second, now));
}

void TestVoiceLifecycle::outboxKeepsTheCandidateBatchUntilItIsAccepted()
{
    // Candidates are sent exactly ONCE by flushCandidateBatch, so before
    // the outbox a single failed PUT lost them permanently and the peer
    // never learned a route. The payload has to survive the failure.
    voice::CallEventOutbox outbox;
    const QByteArray batch("{\"candidates\":[{\"candidate\":\"a\"}]}");
    const qint64 t0 = 5'000;

    const quint64 token = outbox.add("m.call.candidates", batch, t0);
    QCOMPARE(outbox.due(t0).first().payload, batch);
    QVERIFY(outbox.onFailed(token, t0));
    // Same bytes, not a truncated or empty re-send.
    QCOMPARE(outbox.due(t0 + 500).first().payload, batch);

    // A later batch queues behind it rather than replacing it: both sets
    // of candidates are needed, and ICE has no way to ask again.
    const quint64 later = outbox.add("m.call.candidates",
                                     QByteArray("{\"candidates\":[]}"),
                                     t0 + 600);
    QCOMPARE(outbox.pendingCount(), 2);
    outbox.onSent(later);
    QCOMPARE(outbox.pendingCount(), 1);

    // The session ending drops whatever is left — there is no call to
    // send it into.
    outbox.clear();
    QVERIFY(outbox.isEmpty());
}

void TestVoiceLifecycle::leavingFlushesTheHangupsStopItselfQueued()
{
    // VoiceEngine::stop() clears m_running and THEN enqueues one
    // m.call.hangup per peer, which the outbox flush gate refused
    // because the engine was no longer running — and two statements
    // later stop() cleared the outbox. Leaving a voice channel therefore
    // sent nothing to anybody, and each remaining peer held the leaver's
    // connection, mixer row and tile until its own 10 s disconnect grace
    // or 30 s setup watchdog expired.
    //
    // The gate now takes a third input so the session that is stopping
    // may still drain what stop() queued.

    // The regression: running is already false when the hangups go in.
    QVERIFY(voice::mayFlushCallEvents(/*running=*/false, /*stopping=*/true,
                                      /*haveRoom=*/true));
    // A stopped, non-stopping engine stays silent: nothing queued after
    // stop() returns belongs to a call that still exists.
    QVERIFY(!voice::mayFlushCallEvents(false, false, true));
    // Normal mid-call sending is unchanged.
    QVERIFY(voice::mayFlushCallEvents(true, false, true));
    // No room means no endpoint to PUT to, whatever the other two say.
    QVERIFY(!voice::mayFlushCallEvents(true, false, false));
    QVERIFY(!voice::mayFlushCallEvents(false, true, false));

    // And the queue itself hands the hangups over on that first due()
    // call, before stop()'s clear() discards the rest — i.e. one flush
    // inside the stopping window is enough for every peer.
    voice::CallEventOutbox outbox;
    const qint64 t0 = 1'000;
    for (const char* peer : {"@a:test", "@b:test", "@c:test"}) {
        outbox.add("m.call.hangup",
                   QByteArray("{\"to\":\"") + peer + "\"}", t0);
    }
    QCOMPARE(outbox.pendingCount(), 3);
    const auto due = outbox.due(t0);
    QCOMPARE(due.size(), 3);
    for (const auto& entry : due)
        QCOMPARE(entry.type, QStringLiteral("m.call.hangup"));
    outbox.clear();
    QVERIFY(outbox.isEmpty());
}

void TestVoiceLifecycle::aFailedPeerKeepsSayingFailedInsteadOfNew()
{
    // VoiceEngine tears a peer down in the same slot that reports it
    // Failed, so the roster saw "failed" and, one statement later, saw
    // the peer leave the map — after which the lookup missed and fell
    // back to the default. A peer whose ICE had failed therefore
    // rendered identically to one that had only just been added, while
    // the mesh reconciler quietly retried every 5 s from one side only.

    // While a peer object exists, its own state is the answer.
    QCOMPARE(voice::peerDisplayState(QStringLiteral("connected"), false),
             QStringLiteral("connected"));
    QCOMPARE(voice::peerDisplayState(QStringLiteral("connecting"), false),
             QStringLiteral("connecting"));
    // ...even a live peer that is itself reporting failure.
    QCOMPARE(voice::peerDisplayState(QStringLiteral("failed"), false),
             QStringLiteral("failed"));

    // No peer and no history: genuinely new (a roster member we have
    // not offered to yet, e.g. one whose id sorts below ours).
    QCOMPARE(voice::peerDisplayState(QString(), false),
             QStringLiteral("new"));

    // THE REGRESSION: no peer, because we gave up on it.
    QCOMPARE(voice::peerDisplayState(QString(), true),
             QStringLiteral("failed"));

    // A live state always wins over the memory of an earlier failure —
    // that is what makes a successful re-offer clear the indicator
    // without a second code path.
    QCOMPARE(voice::peerDisplayState(QStringLiteral("connecting"), true),
             QStringLiteral("connecting"));
}

void TestVoiceLifecycle::joiningWhileDeafenedDeafensTheNewEngine()
{
    // Deafen reached the transport only through ServerConnection's
    // deafenedChanged handler, which by definition cannot fire when the
    // value has not changed. Every join builds a BRAND NEW VoiceEngine
    // at the AudioEngine default (undeafened), so a user who was already
    // deafened — because they deafened before joining, or deafened in
    // one channel and switched to another — heard everybody, while the
    // UI and the server both said they were deafened. Mute already had
    // this covered (V-M7, the m_appliedGate reset in onTurnConfig);
    // deafen had no hook at all.
    VoiceSession s;
    s.setLocalUserId("@me:x");
    FakeMatrixClient net(&s);
    FakeEngine engine;
    engine.install(&s);

    // Deafen before joining anything.
    s.toggleDeafen();
    QVERIFY(s.deafened());
    QCOMPARE(engine.deafenGates, QVector<bool>{true});

    joinTo(s, net, "!room:x");
    QCOMPARE(s.state(), VoiceSession::State::Active);
    QCOMPARE(engine.starts, 1);
    // THE REGRESSION: the fresh engine is told, even though the value
    // has not changed since the last time anything was told.
    QCOMPARE(engine.deafenGates, (QVector<bool>{true, true}));
    // The join also announced the non-default state to the server, the
    // way the mic gate already did.
    net.replyStateOk("!room:x");

    // Mid-call toggles still reach it, and still only on a change.
    s.toggleDeafen();
    QVERIFY(!s.deafened());
    QCOMPARE(engine.deafenGates, (QVector<bool>{true, true, false}));
    net.replyStateOk("!room:x");

    // Channel switch: another new engine, so the state is pushed again
    // even though the value is unchanged.
    s.requestJoin("!other:x");
    net.replyLeaveOk("!room:x");
    net.replyJoinOk("!other:x");
    net.replyTurn(turnConfig());
    QCOMPARE(s.state(), VoiceSession::State::Active);
    QCOMPARE(engine.starts, 2);
    QCOMPARE(engine.deafenGates, (QVector<bool>{true, true, false, false}));
}

void TestVoiceLifecycle::transportSelectorRefusesMixedRoster()
{
    // V-L4. The rule existed and was tested, but nothing called it from
    // joinVoiceChannel — so the mesh/SFU refusal was dead code. This
    // pins the two inputs the join path actually passes.
    voice::TransportInputs in;
    in.localUserId = QStringLiteral("@me:x");
    in.clientSupportsLiveKit = false;          // no client transport exists yet
    in.serverOfferedLiveKitToken = true;
    QJsonObject sfuPeer;
    sfuPeer["user_id"] = QStringLiteral("@other:x");
    sfuPeer["active"] = true;
    sfuPeer["transport"] = QStringLiteral("livekit");
    in.voiceMembers = QJsonArray{sfuPeer};

    const auto decision = voice::chooseTransport(in);
    QVERIFY(decision.refused());
    QVERIFY(!decision.refusalReason.isEmpty());

    in.voiceMembers = QJsonArray();
    QVERIFY(voice::chooseTransport(in).isMesh());
}

QTEST_MAIN(TestVoiceLifecycle)
#include "test_voice_lifecycle.moc"
