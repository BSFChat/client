#include "net/VoiceSession.h"

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
    m_turnRefreshTimer = new QTimer(this);
    m_turnRefreshTimer->setSingleShot(true);
    connect(m_turnRefreshTimer, &QTimer::timeout, this, [this]() {
        // Only meaningful while a session is live: the refreshed config
        // is for peer connections built from here on (V-M3).
        if (m_state != State::Active) return;
        qCInfo(logVoiceSession, "TURN credentials at half-life — refetching");
        emit turnConfigRequested();
    });
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

// ---------------------------------------------------------------------
// Request queue
// ---------------------------------------------------------------------

VoiceSession::Op VoiceSession::makeOp(OpKind kind, const QString& roomId) const
{
    Op op;
    op.kind = kind;
    op.roomId = roomId;
    // A leave or a state PUT must carry the token of the membership it is
    // about. A JOIN cannot: it is the request that mints the next one.
    if (kind != OpKind::Join && kind != OpKind::Rejoin) op.sessionId = m_sessionId;
    return op;
}

VoiceSession::Op VoiceSession::stateOp(const QString& roomId) const
{
    Op op = makeOp(OpKind::StateUpdate, roomId);
    // V-M7: what we announce is the gate we actually apply, never the raw
    // mute toggle.
    op.muted = effectiveMuted();
    op.deafened = m_deafened;
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
    // Strictly one server-mutating voice request outstanding at a time.
    // This is the whole of the V-H1 fix: a join queued behind a leave
    // cannot reach the server until the leave has been answered, so the
    // server can never apply them out of order.
    if (m_inFlight) return;
    if (m_queue.isEmpty()) {
        if (m_state == State::Idle) emit settled();
        return;
    }

    Op op = m_queue.dequeue();
    m_inFlight = op;

    switch (op.kind) {
    case OpKind::Join:
        m_roomId = op.roomId;
        m_members = QJsonArray();
        m_rejoinAttempted = false;
        m_buffered.clear();
        // The server writes a FRESH m.call.member row on join, with
        // muted/deafened false. Our own preference is deliberately not
        // reset (V-M7: mute must survive a channel switch) — instead the
        // acknowledged state goes back to the server's default so the
        // difference is noticed and announced once we are active.
        m_ackedMuted = false;
        m_ackedDeafened = false;
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
        m_ackedMuted = false;
        m_ackedDeafened = false;
        emit joinRequested(op.roomId);
        break;
    case OpKind::Leave:
        // Local teardown happens when the leave goes OUT, never on its
        // reply: the reply must not be load-bearing for engine cleanup
        // (a lost reply would otherwise leave the mic live forever).
        if (m_stopEngine) m_stopEngine();
        m_turnRefreshTimer->stop();
        m_buffered.clear();
        setState(State::Leaving);
        // The token goes out with the request, not read at reply time:
        // by then it may already belong to the next membership.
        emit leaveRequested(op.roomId, op.sessionId);
        m_sessionId.clear();
        break;
    case OpKind::StateUpdate:
        emit stateUpdateRequested(op.roomId, op.muted, op.deafened, op.sessionId);
        break;
    case OpKind::MediaUpdate:
        emit mediaStateUpdateRequested(op.roomId, op.screenSharing, op.cameraOn,
                                       op.sessionId);
        break;
    }
}

void VoiceSession::completeOp()
{
    m_inFlight.reset();
    pump();
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

void VoiceSession::requestJoin(const QString& roomId)
{
    if (roomId.isEmpty()) return;

    const QString dest = destinationRoom();
    if (dest == roomId) return;  // already there, or already on the way

    // Channel switch: the leave goes in FRONT of the join and the local
    // engine dies when the leave is ISSUED, not when its reply lands. The
    // queue then holds the join back until the server has answered the
    // leave — which is the V-H1 fix. Sending both at once let the server
    // apply join-then-leave and leave the user a ghost.
    if (!dest.isEmpty())
        m_queue.enqueue(makeOp(OpKind::Leave, dest));

    enqueue(makeOp(OpKind::Join, roomId));
}

void VoiceSession::requestLeave()
{
    const QString dest = destinationRoom();
    // Drop anything queued that only makes sense inside the session we
    // are about to abandon.
    QQueue<Op> keep;
    while (!m_queue.isEmpty()) {
        Op op = m_queue.dequeue();
        if (op.kind == OpKind::Leave) keep.enqueue(op);
    }
    m_queue = keep;
    if (dest.isEmpty()) { pump(); return; }
    enqueue(makeOp(OpKind::Leave, dest));
}

void VoiceSession::toggleMute()
{
    m_muted = !m_muted;
    emit mutedChanged();
    applyMicGate();
    if (m_state == State::Active || m_state == State::FetchingTurn) {
        // Announce the EFFECTIVE gate, not the raw toggle (V-M7).
        enqueue(stateOp(m_roomId));
    }
}

void VoiceSession::toggleDeafen()
{
    m_deafened = !m_deafened;
    emit deafenedChanged();
    applyDeafenGate();
    if (m_state == State::Active || m_state == State::FetchingTurn) {
        enqueue(stateOp(m_roomId));
    }
}

void VoiceSession::setPttMode(bool ptt)
{
    if (m_pttMode == ptt) return;
    m_pttMode = ptt;
    const bool before = m_appliedGate.value_or(false);
    applyMicGate();
    if (m_state == State::Active && before != effectiveMuted()) {
        enqueue(stateOp(m_roomId));
    }
}

void VoiceSession::setPttPressed(bool pressed)
{
    if (m_pttPressed == pressed) return;
    m_pttPressed = pressed;
    const bool before = m_appliedGate.value_or(false);
    applyMicGate();
    // Announce the gate change so the roster's mute pip matches what is
    // actually being transmitted. Only in PTT mode does the press move
    // the gate at all.
    if (m_pttMode && m_state == State::Active && before != effectiveMuted()) {
        enqueue(stateOp(m_roomId));
    }
}

void VoiceSession::announceMedia(bool screenSharing, bool cameraOn)
{
    if (m_state != State::Active) return;
    Op op = makeOp(OpKind::MediaUpdate, m_roomId);
    op.screenSharing = screenSharing;
    op.cameraOn = cameraOn;
    enqueue(op);
}

// ---------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------

void VoiceSession::onJoinSucceeded(const QString& roomId,
                                   const QJsonArray& members,
                                   const QString& sessionId)
{
    if (inFlightIs(OpKind::Rejoin, roomId)) {
        m_sessionId = sessionId;
        // V-H2 recovery landed: our row is back, the engine never
        // stopped.
        m_members = members;
        qCInfo(logVoiceSession, "re-join accepted for %s", qPrintable(roomId));
        completeOp();
        if (effectiveMuted() != m_ackedMuted || m_deafened != m_ackedDeafened) {
            enqueue(stateOp(roomId));
        }
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
    // TURN is fetched with the join op still in flight, so a leave or a
    // state PUT queued during the round trip cannot overtake it.
    emit turnConfigRequested();
}

void VoiceSession::onJoinFailed(const QString& roomId, const QString& error)
{
    if (inFlightIs(OpKind::Rejoin, roomId)) {
        // The server refused to reinstate us: we really are out.
        qCWarning(logVoiceSession, "re-join refused for %s — tearing down",
                  qPrintable(roomId));
        if (m_stopEngine) m_stopEngine();
        m_roomId.clear();
        m_sessionId.clear();
        m_members = QJsonArray();
        m_buffered.clear();
        m_turnRefreshTimer->stop();
        setState(State::Idle);
        emit errorOccurred(error.isEmpty()
            ? QStringLiteral("You were removed from the voice channel")
            : error);
        completeOp();
        return;
    }
    if (!inFlightIs(OpKind::Join, roomId)) return;
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
        // A half-life refresh (V-M3): hand the new credentials to the
        // running transport and re-arm.
        m_turnConfig = config;
        if (m_turnUpdater) m_turnUpdater(config);
        scheduleTurnRefresh(config);
        return;
    }
    if (m_state != State::FetchingTurn) return;

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
    // engine at the AudioEngine default. Without the reset+apply a user
    // who was already deafened before joining — or who deafened in the
    // previous channel and switched — would hear everyone, because the
    // only thing that ever pushed deafen into the engine was a CHANGE in
    // its value, and nothing changed.
    m_appliedDeafen.reset();
    applyDeafenGate();
    completeOp();
    if (effectiveMuted() != m_ackedMuted || m_deafened != m_ackedDeafened) {
        enqueue(stateOp(roomId));
    }
}

void VoiceSession::onTurnConfigFailed(const QString& error)
{
    if (m_state == State::Active) {
        // A failed half-life refresh is not fatal: the existing peers
        // keep their credentials and the next attempt is one ttl/2 away.
        qCWarning(logVoiceSession, "TURN refresh failed — keeping existing "
                  "credentials");
        scheduleTurnRefresh(m_turnConfig);
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
    if (m_stopEngine) m_stopEngine();
    m_buffered.clear();
    m_members = QJsonArray();
    m_turnRefreshTimer->stop();
    m_roomId.clear();
    m_sessionId.clear();
    setState(State::Idle);
    if (!reason.isEmpty()) emit errorOccurred(reason);
    // Tell the server to drop the membership. Pushed to the FRONT of the
    // queue: it must precede a join the user has already asked for, or
    // the two race exactly as V-H1 did.
    if (!roomId.isEmpty())
        m_queue.prepend(makeOp(OpKind::Leave, roomId));
    completeOp();
}

void VoiceSession::onStateUpdateSucceeded(const QString& roomId)
{
    if (!inFlightIs(OpKind::StateUpdate, roomId)
        && !inFlightIs(OpKind::MediaUpdate, roomId)) {
        return;
    }
    if (m_inFlight->kind == OpKind::StateUpdate) {
        m_ackedMuted = m_inFlight->muted;
        m_ackedDeafened = m_inFlight->deafened;
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
    onSelfMembership(roomId, false, m_sessionId);
}

void VoiceSession::onStateUpdateFailed(const QString& roomId,
                                       const QString& error)
{
    if (!inFlightIs(OpKind::StateUpdate, roomId)
        && !inFlightIs(OpKind::MediaUpdate, roomId)) {
        return;
    }
    const bool wasMuteUpdate = m_inFlight->kind == OpKind::StateUpdate;
    completeOp();
    if (!wasMuteUpdate) {
        if (!error.isEmpty()) emit errorOccurred(error);
        return;
    }
    // V-M7: the local toggle was optimistic. The server did not take it,
    // so put the UI back where the server thinks we are rather than
    // leaving a mute pip that lies.
    bool changedMute = false;
    bool changedDeafen = false;
    if (m_muted != m_ackedMuted) { m_muted = m_ackedMuted; changedMute = true; }
    if (m_deafened != m_ackedDeafened) {
        m_deafened = m_ackedDeafened;
        changedDeafen = true;
    }
    if (changedMute) emit mutedChanged();
    if (changedDeafen) emit deafenedChanged();
    if (changedMute || changedDeafen) applyMicGate();
    emit errorOccurred(error.isEmpty()
        ? QStringLiteral("Could not update your voice state")
        : error);
}

void VoiceSession::onMembersPolled(const QString& roomId,
                                   const QJsonArray& members)
{
    if (roomId != m_roomId) return;
    m_members = members;
}

void VoiceSession::onSelfMembership(const QString& roomId, bool active,
                                    const QString& sessionId)
{
    if (!active && !sessionId.isEmpty() && !m_sessionId.isEmpty()
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
    if (active) {
        // Our row is healthy again; a later retirement gets a fresh
        // recovery attempt.
        if (roomId == m_roomId) m_rejoinAttempted = false;
        return;
    }
    if (m_state != State::Active || roomId != m_roomId) return;
    if (m_rejoinAttempted) {
        qCWarning(logVoiceSession, "retired from %s again after a re-join — "
                  "tearing down", qPrintable(roomId));
        if (m_stopEngine) m_stopEngine();
        m_roomId.clear();
        m_sessionId.clear();
        m_members = QJsonArray();
        m_buffered.clear();
        m_turnRefreshTimer->stop();
        setState(State::Idle);
        emit errorOccurred(
            QStringLiteral("You were disconnected from the voice channel"));
        return;
    }
    m_rejoinAttempted = true;
    qCWarning(logVoiceSession, "server retired our voice row in %s — "
              "re-joining once", qPrintable(roomId));
    enqueue(makeOp(OpKind::Rejoin, roomId));
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
    // V-M3. The reconciler re-offers minutes into a call and builds fresh
    // peer connections from these credentials; a TURN username/password
    // pair is only valid for `ttl` seconds.
    int ttl = config.value(QStringLiteral("ttl")).toInt(kDefaultTurnTtlSeconds);
    if (ttl <= 0) ttl = kDefaultTurnTtlSeconds;
    const int halfLifeMs = (ttl / 2) * 1000;
    m_turnRefreshTimer->start(halfLifeMs > 1000 ? halfLifeMs : 1000);
}

void VoiceSession::abandon()
{
    m_queue.clear();
    m_inFlight.reset();
    m_buffered.clear();
    m_members = QJsonArray();
    m_turnRefreshTimer->stop();
    if (m_state != State::Idle && m_stopEngine) m_stopEngine();
    m_roomId.clear();
    m_sessionId.clear();
    setState(State::Idle);
}
