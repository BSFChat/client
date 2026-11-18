#include "net/VoiceSession.h"

#include "voice/TurnCredentials.h"

#include <QDateTime>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(logVoiceSession, "bsfchat.voice.session", QtWarningMsg)

namespace {
constexpr char kCallInvite[] = "m.call.invite";
}

const char* VoiceSession::stateName(State s)
{
    switch (s) {
    case State::Idle: return "Idle";
    case State::Joining: return "Joining";
    case State::FetchingTurn: return "FetchingTurn";
    case State::Active: return "Active";
    case State::Leaving: return "Leaving";
    }
    return "?";
}

VoiceSession::VoiceSession(QObject* parent)
    : QObject(parent)
{
    m_monotonic.start();

    // TURN refresh: a short wall-clock check, re-armed each time, rather
    // than one long monotonic timer that sleep can stretch (V2-7).
    m_turnRefreshTimer = new QTimer(this);
    m_turnRefreshTimer->setSingleShot(true);
    connect(m_turnRefreshTimer, &QTimer::timeout, this,
            &VoiceSession::checkTurnRefresh);

    m_stateRetryTimer = new QTimer(this);
    m_stateRetryTimer->setSingleShot(true);
    connect(m_stateRetryTimer, &QTimer::timeout, this,
            &VoiceSession::maybeSendState);
    m_stateSpacingTimer = new QTimer(this);
    m_stateSpacingTimer->setSingleShot(true);
    connect(m_stateSpacingTimer, &QTimer::timeout, this,
            &VoiceSession::maybeSendState);

    m_rejoinGraceTimer = new QTimer(this);
    m_rejoinGraceTimer->setSingleShot(true);
    connect(m_rejoinGraceTimer, &QTimer::timeout, this, [this]() {
        const QString room = m_pendingRejoinRoom;
        m_pendingRejoinRoom.clear();
        if (room.isEmpty() || m_state != State::Active || room != m_roomId)
            return;
        enqueue(makeOp(OpKind::Rejoin, room));
    });
}

QString VoiceSession::takenOverMessage()
{
    return QStringLiteral("You joined this call from another device");
}

qint64 VoiceSession::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void VoiceSession::setState(State s)
{
    if (m_state == s) return;
    qCInfo(logVoiceSession, "%s → %s (room=%s)", stateName(m_state),
           stateName(s), qPrintable(m_roomId));
    m_state = s;
    emit stateChanged();
}

bool VoiceSession::effectiveMuted() const
{
    return m_muted || (m_pttMode && !m_pttPressed);
}

void VoiceSession::applyMicGate()
{
    const bool gate = effectiveMuted();
    if (m_appliedGate && *m_appliedGate == gate) return;
    m_appliedGate = gate;
    if (m_micGate) m_micGate(gate);
}

void VoiceSession::applyDeafenGate()
{
    if (m_appliedDeafen && *m_appliedDeafen == m_deafened) return;
    m_appliedDeafen = m_deafened;
    if (m_deafenGate) m_deafenGate(m_deafened);
}

void VoiceSession::stopEngineOnce()
{
    if (!m_localLive) return;
    m_localLive = false;
    if (m_stopEngine) m_stopEngine();
}

// ---------------------------------------------------------------------
// Request queue (join / leave)
// ---------------------------------------------------------------------

VoiceSession::Op VoiceSession::makeOp(OpKind kind, const QString& roomId) const
{
    Op op;
    op.kind = kind;
    op.roomId = roomId;
    // A leave must carry the token of the membership it is about. A JOIN
    // cannot: it is the request that mints the next one.
    if (kind == OpKind::Leave) op.sessionId = m_sessionId;
    return op;
}

void VoiceSession::enqueue(Op op)
{
    m_queue.enqueue(std::move(op));
    pump();
}

bool VoiceSession::inFlightIs(OpKind kind, const QString& roomId) const
{
    return m_inFlight && m_inFlight->kind == kind
        && (roomId.isEmpty() || m_inFlight->roomId == roomId);
}

void VoiceSession::pump()
{
    // Strictly one membership request outstanding at a time. This is the
    // whole of the V-H1 fix: a join queued behind a leave cannot reach the
    // server until the leave has been answered, so the server can never
    // apply them out of order.
    if (m_inFlight) return;
    if (m_queue.isEmpty()) {
        if (m_state == State::Idle) emit settled();
        // A join/rejoin that just completed may have unblocked a state PUT.
        maybeSendState();
        return;
    }

    Op op = m_queue.dequeue();
    m_inFlight = op;

    switch (op.kind) {
    case OpKind::Join:
        m_roomId = op.roomId;
        m_members = QJsonArray();
        m_rejoinAttempted = false;
        m_pendingRejoinRoom.clear();
        m_rejoinGraceTimer->stop();
        m_buffered.clear();
        m_localLive = true;
        // The server writes a FRESH m.call.member row on join, with
        // muted/deafened false. Our own preference is deliberately not
        // reset (V-M7: mute must survive a channel switch) — instead the
        // acknowledged state goes back to the server's default so the
        // difference is noticed and announced once we are active.
        resetStateChannel();
        // Everything stamped before this instant belongs to a previous
        // session (V-M1). Recorded at the moment the request goes out
        // rather than when it is queued, so a join that waited behind a
        // leave is not credited with the wait.
        m_sessionStartMs = QDateTime::currentMSecsSinceEpoch();
        setState(State::Joining);
        emit joinRequested(op.roomId);
        break;
    case OpKind::Rejoin:
        // V-H2 re-POST. State stays Active: the engine is running and
        // the peers are fine; it is the server's row that went away.
        resetStateChannel();
        emit joinRequested(op.roomId);
        break;
    case OpKind::Leave:
        // Local teardown happens before the leave goes out (normally in
        // requestLeave already), never on its reply: the reply must not
        // be load-bearing for engine cleanup.
        stopEngineOnce();
        m_turnRefreshTimer->stop();
        m_buffered.clear();
        setState(State::Leaving);
        // The token goes out with the request, not read at reply time:
        // by then it may already belong to the next membership.
        emit leaveRequested(op.roomId, op.sessionId);
        m_sessionId.clear();
        break;
    }
}

void VoiceSession::completeOp()
{
    m_inFlight.reset();
    pump();
}

// ---------------------------------------------------------------------
// State channel (mute / deafen) — V2-1
// ---------------------------------------------------------------------

void VoiceSession::resetStateChannel()
{
    // A new membership: the server's row starts at muted=false,
    // deafened=false. Any PUT still in flight belongs to the old row.
    if (m_statePut) {
        ++m_orphanedStateReplies[m_statePut->roomId];
        m_statePut.reset();
    }
    m_ackedMuted = false;
    m_ackedDeafened = false;
    m_ackedManualMuted = false;
    m_stateGaveUp = false;
    m_stateRetryMs = 0;
    m_stateRetryTimer->stop();
    m_stateSpacingTimer->stop();
}

void VoiceSession::maybeSendState()
{
    // Only a live, acknowledged membership can take a state PUT, and never
    // while a join/rejoin for it is unanswered (it would be refused as a
    // non-member, or land on the row the re-join is about to replace).
    if (m_state != State::Active) return;
    if (m_inFlight) return;   // a re-join: wait for its fresh row
    if (m_statePut) return;               // one at a time; reply re-runs this
    if (m_stateRetryTimer->isActive()) return;
    const bool wantMuted = effectiveMuted();
    if (wantMuted == m_ackedMuted && m_deafened == m_ackedDeafened) {
        m_stateRetryMs = 0;
        return;
    }
    if (m_stateGaveUp) return;

    const qint64 now = m_monotonic.elapsed();
    if (m_lastStatePutMs > 0 && m_minStatePutIntervalMs > 0) {
        const qint64 due = m_lastStatePutMs + m_minStatePutIntervalMs;
        if (now < due) {
            if (!m_stateSpacingTimer->isActive())
                m_stateSpacingTimer->start(int(due - now));
            return;
        }
    }

    StatePut put;
    put.roomId = m_roomId;
    put.muted = wantMuted;
    put.deafened = m_deafened;
    put.manualMuted = m_muted;
    put.actionSeq = m_userActionSeq;
    m_statePut = put;
    m_lastStatePutMs = qMax<qint64>(1, now);
    emit stateUpdateRequested(put.roomId, put.muted, put.deafened, m_sessionId);
}

void VoiceSession::onStateUpdateSucceeded(const QString& roomId)
{
    if (int& owed = m_orphanedStateReplies[roomId]; owed > 0) {
        if (--owed == 0) m_orphanedStateReplies.remove(roomId);
        return;
    } else {
        m_orphanedStateReplies.remove(roomId);
    }
    if (!m_statePut || m_statePut->roomId != roomId) return;
    m_ackedMuted = m_statePut->muted;
    m_ackedDeafened = m_statePut->deafened;
    m_ackedManualMuted = m_statePut->manualMuted;
    m_statePut.reset();
    m_stateRetryMs = 0;
    // Whatever the user did while it was in flight goes out now.
    maybeSendState();
}

void VoiceSession::onStateUpdateFailed(const QString& roomId,
                                       const QString& error, bool transient)
{
    if (int& owed = m_orphanedStateReplies[roomId]; owed > 0) {
        if (--owed == 0) m_orphanedStateReplies.remove(roomId);
        return;
    } else {
        m_orphanedStateReplies.remove(roomId);
    }
    if (!m_statePut || m_statePut->roomId != roomId) return;
    const StatePut failed = *m_statePut;
    m_statePut.reset();

    if (transient) {
        // The local toggle is right; the server just did not hear it yet.
        m_stateRetryMs = m_stateRetryMs <= 0
            ? m_stateRetryBaseMs : qMin(m_stateRetryMs * 2, kMaxStateRetryMs);
        qCInfo(logVoiceSession, "voice/state failed transiently — retrying "
               "in %d ms", m_stateRetryMs);
        m_stateRetryTimer->start(m_stateRetryMs);
        return;
    }

    // Permanent. Resending the same thing would fail the same way, so stop
    // until the user does something new or the membership changes.
    m_stateGaveUp = true;
    const bool newerAction = failed.actionSeq != m_userActionSeq;
    if (!newerAction) {
        // Roll back only towards privacy. Putting the UI back where the
        // server thinks we are is honest for an UNmute or UNdeafen the
        // server refused; for a refused mute it would reopen the
        // microphone while the user believes they are muted (V2-1).
        bool changedMute = false;
        bool changedDeafen = false;
        if (!m_muted && m_ackedManualMuted) { m_muted = true; changedMute = true; }
        if (!m_deafened && m_ackedDeafened) { m_deafened = true; changedDeafen = true; }
        if (changedMute) emit mutedChanged();
        if (changedDeafen) emit deafenedChanged();
        // BOTH gates, every time: the transport must match the toggles
        // whatever moved.
        applyMicGate();
        applyDeafenGate();
    }
    emit errorOccurred(error.isEmpty()
        ? QStringLiteral("Could not update your voice state")
        : error);
}

// ---------------------------------------------------------------------
// Intents
// ---------------------------------------------------------------------

QString VoiceSession::destinationRoom() const
{
    // Where the queue, once drained, leaves this session. Answers
    // "are we already on our way there?" without the caller having to
    // reason about in-flight requests.
    QString dest = (m_state == State::Idle || m_state == State::Leaving)
        ? QString() : m_roomId;
    for (const Op& op : m_queue) {
        if (op.kind == OpKind::Join) dest = op.roomId;
        else if (op.kind == OpKind::Leave) dest.clear();
    }
    return dest;
}

QString VoiceSession::leaveLocally()
{
    // Drop anything queued that only makes sense inside the session we
    // are abandoning. Queued leaves stay: the server must still hear them.
    QQueue<Op> keep;
    while (!m_queue.isEmpty()) {
        Op op = m_queue.dequeue();
        if (op.kind == OpKind::Leave) keep.enqueue(op);
    }
    m_queue = keep;
    m_pendingRejoinRoom.clear();
    m_rejoinGraceTimer->stop();

    // The room the server holds (or may be about to hold) us in.
    QString serverRoom;
    if (m_inFlight && (m_inFlight->kind == OpKind::Join
                       || m_inFlight->kind == OpKind::Rejoin)) {
        serverRoom = m_inFlight->roomId;
    } else if (m_state == State::Joining || m_state == State::FetchingTurn
               || m_state == State::Active) {
        serverRoom = m_roomId;
    }
    if (serverRoom.isEmpty()) return QString();

    // Local teardown NOW, whatever is on the wire: mic gate closed with
    // the transport, peers hung up. Leave used to wait for an in-flight
    // join (up to its timeout) with the microphone still live.
    stopEngineOnce();
    m_turnRefreshTimer->stop();
    m_turnRefreshInFlight = false;
    m_buffered.clear();

    if (m_inFlight && m_inFlight->kind == OpKind::Join
        && m_state == State::FetchingTurn) {
        // The join itself was answered (we have its token); only the TURN
        // GET is outstanding, and nothing waits for a GET.
        m_inFlight.reset();
    } else if (m_inFlight && (m_inFlight->kind == OpKind::Join
                              || m_inFlight->kind == OpKind::Rejoin)) {
        // Unanswered: the reply decides nothing any more except the token
        // the leave below should carry.
        m_inFlight->cancelled = true;
    }
    // A mute PUT in flight is not worth waiting for either.
    if (m_statePut) {
        ++m_orphanedStateReplies[m_statePut->roomId];
        m_statePut.reset();
    }
    m_stateRetryTimer->stop();
    m_stateSpacingTimer->stop();

    setState(State::Leaving);
    return serverRoom;
}

void VoiceSession::requestJoin(const QString& roomId)
{
    if (roomId.isEmpty()) return;

    const QString dest = destinationRoom();
    if (dest == roomId) return;  // already there, or already on the way

    // Channel switch: the leave goes in FRONT of the join and the local
    // engine dies NOW, not when the leave's reply lands. The queue then
    // holds the join back until the server has answered the leave — which
    // is the V-H1 fix. Sending both at once let the server apply
    // join-then-leave and leave the user a ghost.
    if (!dest.isEmpty()) {
        const QString serverRoom = leaveLocally();
        if (!serverRoom.isEmpty())
            m_queue.enqueue(makeOp(OpKind::Leave, serverRoom));
    }
    enqueue(makeOp(OpKind::Join, roomId));
}

void VoiceSession::requestLeave()
{
    const QString serverRoom = leaveLocally();
    if (serverRoom.isEmpty()) {
        // Nothing held server-side (e.g. Idle with only a queued join,
        // which leaveLocally just dropped).
        if (m_state == State::Leaving && !m_inFlight && m_queue.isEmpty()) {
            m_roomId.clear();
            setState(State::Idle);
        }
        pump();
        return;
    }
    enqueue(makeOp(OpKind::Leave, serverRoom));
}

void VoiceSession::toggleMute()
{
    m_muted = !m_muted;
    ++m_userActionSeq;
    m_stateGaveUp = false;
    emit mutedChanged();
    applyMicGate();
    maybeSendState();
}

void VoiceSession::toggleDeafen()
{
    m_deafened = !m_deafened;
    ++m_userActionSeq;
    m_stateGaveUp = false;
    emit deafenedChanged();
    applyDeafenGate();
    maybeSendState();
}

void VoiceSession::setPttMode(bool ptt)
{
    if (m_pttMode == ptt) return;
    m_pttMode = ptt;
    applyMicGate();
    maybeSendState();
}

void VoiceSession::setPttPressed(bool pressed)
{
    if (m_pttPressed == pressed) return;
    m_pttPressed = pressed;
    // The gate is local and immediate — that is what decides whether a
    // word is clipped. The roster follows through the coalesced, spaced
    // state channel, so a burst of presses costs at most one PUT per
    // m_minStatePutIntervalMs and the final one always carries the truth.
    applyMicGate();
    if (m_pttMode) maybeSendState();
}

void VoiceSession::announceMedia(bool screenSharing, bool cameraOn)
{
    if (m_state != State::Active) return;
    emit mediaStateUpdateRequested(m_roomId, screenSharing, cameraOn,
                                   m_sessionId);
}

// ---------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------

void VoiceSession::onJoinSucceeded(const QString& roomId,
                                   const QJsonArray& members,
                                   const QString& sessionId)
{
    if ((inFlightIs(OpKind::Join, roomId) || inFlightIs(OpKind::Rejoin, roomId))
        && m_inFlight->cancelled) {
        // The user left while this was on the wire. Start nothing; the
        // queued leave must name the membership the server just created,
        // or it would be refused as stale and leave a ghost.
        for (Op& op : m_queue) {
            if (op.kind == OpKind::Leave && op.roomId == roomId) {
                if (!sessionId.isEmpty()) op.sessionId = sessionId;
                break;
            }
        }
        qCInfo(logVoiceSession, "join reply for %s arrived after leave — "
               "not starting", qPrintable(roomId));
        completeOp();
        return;
    }
    if (inFlightIs(OpKind::Rejoin, roomId)) {
        m_sessionId = sessionId;
        // V-H2 recovery landed: our row is back, the engine never
        // stopped.
        m_members = members;
        qCInfo(logVoiceSession, "re-join accepted for %s", qPrintable(roomId));
        completeOp();
        // New membership, so the server's media flags are back to false.
        // Whoever knows what we are actually capturing has to say so again.
        emit membershipRenewed(roomId);
        // A re-join is typically what follows a sleep: the credentials the
        // engine holds may well have expired with it (V2-7).
        requestTurnRefresh("re-join");
        return;
    }
    if (!inFlightIs(OpKind::Join, roomId)) {
        qCInfo(logVoiceSession, "ignoring stale join reply for %s",
               qPrintable(roomId));
        return;
    }
    m_members = members;
    m_sessionId = sessionId;
    setState(State::FetchingTurn);
    // TURN is fetched with the join op still in flight, so a leave
    // queued during the round trip cannot overtake it.
    emit turnConfigRequested();
}

void VoiceSession::endSession(const QString& message)
{
    stopEngineOnce();
    m_roomId.clear();
    m_sessionId.clear();
    m_members = QJsonArray();
    m_buffered.clear();
    m_turnRefreshTimer->stop();
    m_turnRefreshInFlight = false;
    m_pendingRejoinRoom.clear();
    m_rejoinGraceTimer->stop();
    if (m_statePut) {
        ++m_orphanedStateReplies[m_statePut->roomId];
        m_statePut.reset();
    }
    m_stateRetryTimer->stop();
    m_stateSpacingTimer->stop();
    setState(State::Idle);
    if (!message.isEmpty()) emit errorOccurred(message);
}

void VoiceSession::onJoinFailed(const QString& roomId, const QString& error)
{
    if ((inFlightIs(OpKind::Join, roomId) || inFlightIs(OpKind::Rejoin, roomId))
        && m_inFlight->cancelled) {
        // Already left locally; the queued leave still goes out (the
        // server accepts a leave from a non-member, and a timed-out join
        // may have landed after all).
        completeOp();
        return;
    }
    if (inFlightIs(OpKind::Rejoin, roomId)) {
        // The server refused to reinstate us: we really are out.
        qCWarning(logVoiceSession, "re-join refused for %s — tearing down",
                  qPrintable(roomId));
        endSession(error.isEmpty()
            ? QStringLiteral("You were removed from the voice channel")
            : error);
        completeOp();
        return;
    }
    if (!inFlightIs(OpKind::Join, roomId)) return;
    m_localLive = false;   // nothing was started for a refused join
    m_roomId.clear();
    m_sessionId.clear();
    m_buffered.clear();
    setState(State::Idle);
    if (!error.isEmpty()) emit errorOccurred(error);
    completeOp();
}

void VoiceSession::onLeaveSucceeded(const QString& roomId)
{
    if (!inFlightIs(OpKind::Leave, roomId)) return;
    if (m_state == State::Leaving) {
        m_roomId.clear();
        setState(State::Idle);
    }
    completeOp();
}

void VoiceSession::onLeaveFailed(const QString& roomId, const QString& error)
{
    if (!inFlightIs(OpKind::Leave, roomId)) return;
    // The local side is already torn down; a failed leave only means the
    // server row lingers until the ghost reaper takes it. Do NOT unwind
    // anything else — in particular, this must not disturb a join queued
    // behind it, which was the V-M4 shape of this bug.
    if (m_state == State::Leaving) {
        m_roomId.clear();
        setState(State::Idle);
    }
    if (!error.isEmpty()) emit errorOccurred(error);
    completeOp();
}

void VoiceSession::onTurnConfig(const QJsonObject& config)
{
    if (m_state == State::Active) {
        // A scheduled/wake refresh: hand the new credentials to the
        // running transport and re-arm.
        m_turnRefreshInFlight = false;
        m_turnConfig = config;
        if (m_turnUpdater) m_turnUpdater(config);
        scheduleTurnRefresh(config);
        return;
    }
    if (m_state != State::FetchingTurn || !inFlightIs(OpKind::Join, m_roomId))
        return;

    m_turnConfig = config;
    const QString roomId = m_roomId;
    const bool ok = m_startEngine ? m_startEngine(roomId, m_members, config)
                                  : true;
    if (!ok) {
        // start() refuses sessions that cannot work (no audio device,
        // relay-only policy with no TURN). It has already reported why.
        unwind(roomId, QString());
        return;
    }

    setState(State::Active);
    scheduleTurnRefresh(config);
    // The engine exists now — hand it everything that arrived while it
    // did not (V-C1). This is the ~35 s of silence.
    replayBufferedSignals();
    // Apply and, if it is not the server's default, announce the mic
    // gate. A user who joins muted (or in PTT mode) must not be listed
    // as unmuted, and mute must survive a channel switch (V-M7).
    m_appliedGate.reset();
    applyMicGate();
    // Same for deafening, and for the same reason: this is a BRAND NEW
    // engine at the AudioEngine default.
    m_appliedDeafen.reset();
    applyDeafenGate();
    completeOp();   // pump() → maybeSendState() announces any difference
}

void VoiceSession::onTurnConfigFailed(const QString& error)
{
    if (m_state == State::Active) {
        // A failed refresh is not fatal: the existing peers keep their
        // allocations. Try again on the next check.
        m_turnRefreshInFlight = false;
        qCWarning(logVoiceSession, "TURN refresh failed — keeping existing "
                  "credentials");
        m_turnRefreshAtMs = nowMs() + 60'000;
        m_turnRefreshTimer->start(qMin(60'000, voice::turn::kCheckIntervalMs));
        return;
    }
    if (m_state != State::FetchingTurn) return;
    // V-C1's sibling: without TURN the join can never produce audio, and
    // the 5 s member poll would keep the server-side heartbeat alive, so
    // the ghost reaper would never remove the user. Unwind completely.
    qCWarning(logVoiceSession, "TURN configuration unavailable — unwinding "
              "join for %s", qPrintable(m_roomId));
    unwind(m_roomId, error);
}

void VoiceSession::unwind(QString roomId, QString reason)
{
    // The join that brought us here was answered; its token is what the
    // server-side leave must name.
    const QString sessionId = m_sessionId;
    stopEngineOnce();
    m_buffered.clear();
    m_members = QJsonArray();
    m_turnRefreshTimer->stop();
    m_roomId.clear();
    m_sessionId.clear();
    setState(State::Idle);
    if (!reason.isEmpty()) emit errorOccurred(reason);
    // Tell the server to drop the membership. Pushed to the FRONT of the
    // queue: it must precede a join the user has already asked for, or
    // the two race exactly as V-H1 did. The join op that brought us here
    // already has its token in hand.
    if (!roomId.isEmpty()) {
        Op leave;
        leave.kind = OpKind::Leave;
        leave.roomId = roomId;
        leave.sessionId = sessionId;
        m_queue.prepend(leave);
    }
    completeOp();
}

void VoiceSession::onStateSuperseded(const QString& roomId)
{
    // 403 from voice/state: the row the server holds carries a newer
    // token than ours. A reaped row is never re-activated by a state PUT,
    // so this takes the same recovery path as a retracted self-row.
    if (m_state != State::Active || roomId != m_roomId) return;
    qCWarning(logVoiceSession, "voice/state refused — our session was "
              "superseded in %s", qPrintable(roomId));
    // The PUT that drew the 403 is dead; its error reply (which follows)
    // must not count as a permanent refusal of the mute itself.
    if (m_statePut && m_statePut->roomId == roomId) {
        ++m_orphanedStateReplies[roomId];
        m_statePut.reset();
    }
    onSelfMembership(roomId, false, m_sessionId);
}

void VoiceSession::onMembersPolled(const QString& roomId,
                                   const QJsonArray& members)
{
    if (roomId != m_roomId) return;
    m_members = members;
}

void VoiceSession::scheduleRejoin(const QString& roomId)
{
    m_rejoinAttempted = true;
    qCWarning(logVoiceSession, "server retired our voice row in %s — "
              "re-joining once%s", qPrintable(roomId),
              m_rejoinGraceMs > 0 ? " unless another device took it" : "");
    if (m_rejoinGraceMs <= 0) {
        enqueue(makeOp(OpKind::Rejoin, roomId));
        return;
    }
    m_pendingRejoinRoom = roomId;
    m_rejoinGraceTimer->start(m_rejoinGraceMs);
}

void VoiceSession::onSelfMembership(const QString& roomId, bool active,
                                    const QString& sessionId,
                                    const QString& deviceId)
{
    if (active) {
        if (roomId != m_roomId) return;
        const bool ourSession = !sessionId.isEmpty() && sessionId == m_sessionId;
        if (ourSession) {
            // Our row is healthy again; a later retirement gets a fresh
            // recovery attempt. ONLY for our own session: resetting on any
            // active row of ours is what let two devices evict each other
            // forever (V2-3).
            m_rejoinAttempted = false;
            return;
        }
        if (sessionId.isEmpty() && m_sessionId.isEmpty()) {
            // Legacy server, no tokens at all: the old behaviour.
            if (deviceId.isEmpty() || m_localDeviceId.isEmpty()
                || deviceId == m_localDeviceId) {
                m_rejoinAttempted = false;
                return;
            }
        }
        if (m_state != State::Active) return;
        // Our own re-join's row can arrive before its reply hands us the
        // token; that is not a takeover.
        if (inFlightIs(OpKind::Rejoin, roomId)) return;
        const bool devicesKnown = !deviceId.isEmpty() && !m_localDeviceId.isEmpty();
        // Same device, different session: an older row of ours replayed
        // (sync resume), not another device.
        if (devicesKnown && deviceId == m_localDeviceId) return;
        const bool otherDevice = devicesKnown && deviceId != m_localDeviceId;
        const bool otherSession = !sessionId.isEmpty() && !m_sessionId.isEmpty()
            && sessionId != m_sessionId;
        if (!otherDevice && !otherSession) return;   // nothing to go on
        qCWarning(logVoiceSession, "our account joined %s from another device "
                  "(session %s) — ending the call here", qPrintable(roomId),
                  qPrintable(sessionId));
        // The server has already retired our row; no leave to send.
        endSession(takenOverMessage());
        emit takenOverByOtherDevice(roomId);
        return;
    }

    if (!sessionId.isEmpty() && !m_sessionId.isEmpty()
        && sessionId != m_sessionId) {
        // A retraction naming a DIFFERENT membership of ours: the server
        // echoes the retracted session on every leave, rejoin-reset and
        // reap, and a join over an active row deliberately emits
        // active=false (naming the OLD session) before active=true so
        // peers get an edge to reset on. Acting on that edge would tear
        // down the session we just established.
        qCInfo(logVoiceSession, "ignoring retraction of superseded session %s",
               qPrintable(sessionId));
        return;
    }
    if (m_state != State::Active || roomId != m_roomId) return;
    if (!m_pendingRejoinRoom.isEmpty() || inFlightIs(OpKind::Rejoin, roomId))
        return;   // recovery already under way
    if (m_rejoinAttempted) {
        qCWarning(logVoiceSession, "retired from %s again after a re-join — "
                  "tearing down", qPrintable(roomId));
        endSession(QStringLiteral("You were disconnected from the voice channel"));
        return;
    }
    scheduleRejoin(roomId);
}

// ---------------------------------------------------------------------
// Inbound signalling
// ---------------------------------------------------------------------

void VoiceSession::routeInboundSignal(const CallSignal& signal)
{
    if (m_roomId.isEmpty() || signal.roomId != m_roomId) return;
    if (!signal.sender.isEmpty() && signal.sender == m_localUserId) return;

    // V-M1: after a failed sync resume the server replays the tail of the
    // room timeline, which for a voice channel is full of old invites
    // carrying dead call ids. Applying one looks like "peer restarted"
    // and tears down a perfectly healthy connection.
    if (signal.type == QLatin1String(kCallInvite) && signal.originServerTs > 0
        && m_sessionStartMs > 0
        && signal.originServerTs < m_sessionStartMs - kClockSkewAllowanceMs) {
        qCInfo(logVoiceSession, "dropping invite from %s stamped before this "
               "session (%lld < %lld)", qPrintable(signal.sender),
               static_cast<long long>(signal.originServerTs),
               static_cast<long long>(m_sessionStartMs));
        return;
    }

    if (m_state == State::Active) {
        if (m_signalSink) m_signalSink(signal);
        return;
    }
    if (m_state == State::Joining || m_state == State::FetchingTurn) {
        // V-C1. The designated offerer invites us the moment it sees our
        // m.call.member, which is well before our TURN fetch returns.
        if (m_buffered.size() >= kMaxBufferedSignals) {
            qCWarning(logVoiceSession, "signal buffer full — dropping %s from "
                      "%s", qPrintable(signal.type), qPrintable(signal.sender));
            return;
        }
        m_buffered.append(signal);
        return;
    }
    // Idle / Leaving: nothing to apply it to.
}

void VoiceSession::replayBufferedSignals()
{
    if (m_buffered.isEmpty()) return;
    qCInfo(logVoiceSession, "replaying %lld signalling events buffered during "
           "join", static_cast<long long>(m_buffered.size()));
    const auto pending = m_buffered;
    m_buffered.clear();
    if (!m_signalSink) return;
    for (const auto& s : pending) m_signalSink(s);
}

void VoiceSession::scheduleTurnRefresh(const QJsonObject& config)
{
    // V-M3 / V2-6 / V2-7. A TURN username/password pair is only valid
    // until its expiry; the refresh is an absolute wall-clock instant,
    // checked by a short tick so a sleep cannot carry us past it.
    const qint64 now = nowMs();
    m_turnExpiresAtMs = voice::turn::expiresAtMs(config, now);
    m_turnRefreshAtMs = voice::turn::refreshAtMs(config, now);
    checkTurnRefresh();
}

void VoiceSession::checkTurnRefresh()
{
    if (m_state != State::Active || m_turnRefreshAtMs <= 0) return;
    const qint64 now = nowMs();
    if (now >= m_turnRefreshAtMs) {
        requestTurnRefresh("credentials due for refresh");
        return;
    }
    const qint64 wait = qMin<qint64>(m_turnRefreshAtMs - now,
                                      voice::turn::kCheckIntervalMs);
    m_turnRefreshTimer->start(int(qMax<qint64>(1, wait)));
}

void VoiceSession::requestTurnRefresh(const char* why)
{
    if (m_state != State::Active || m_turnRefreshInFlight) return;
    qCInfo(logVoiceSession, "refetching TURN credentials (%s)", why);
    m_turnRefreshInFlight = true;
    m_turnRefreshTimer->stop();
    emit turnConfigRequested();
}

void VoiceSession::onWakeOrNetworkChange()
{
    if (m_state != State::Active) return;
    // Refetch when the credentials are expired or about to be — a peer
    // rebuilt for the new network must not allocate with dead ones.
    if (m_turnExpiresAtMs <= 0
        || voice::turn::unusableForNewPeer(m_turnExpiresAtMs, nowMs())
        || nowMs() >= m_turnRefreshAtMs) {
        requestTurnRefresh("wake / network change");
    } else {
        checkTurnRefresh();
    }
}

void VoiceSession::abandon()
{
    m_queue.clear();
    m_inFlight.reset();
    m_buffered.clear();
    m_members = QJsonArray();
    m_turnRefreshTimer->stop();
    m_turnRefreshInFlight = false;
    m_pendingRejoinRoom.clear();
    m_rejoinGraceTimer->stop();
    m_statePut.reset();
    m_orphanedStateReplies.clear();
    m_stateRetryTimer->stop();
    m_stateSpacingTimer->stop();
    stopEngineOnce();
    m_roomId.clear();
    m_sessionId.clear();
    setState(State::Idle);
}
