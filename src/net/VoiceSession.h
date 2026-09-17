#pragma once

// The client-side voice join/leave state machine.
//
// WHY THIS EXISTS AS ITS OWN CLASS
// --------------------------------
// Joining voice used to be a three-legged async dance living inline in
// ServerConnection — `voiceJoined` → `getTurnConfig` → `turnConfigResult`
// → engine — guarded by a single bool (`m_voiceTurnFetchPending`). That
// shape produced four separate defects:
//
//   * V-C1: inbound m.call.* was dropped for the whole TURN round trip,
//     because the dispatcher was gated on `m_voiceEngine != nullptr`. A
//     sitting member who is the designated offerer invites the joiner the
//     instant it sees their m.call.member — roughly 35 s of silence for
//     every pair where the joiner has the greater user id.
//   * V-H1: leave and join went out on parallel connections, so a quick
//     channel switch could land join-before-leave server side and leave
//     the user as a ghost (audible, unlisted, never heartbeating).
//   * V-M4: ANY voiceError inside the TURN window unwound the join,
//     including the previous room's leave error during a channel switch.
//   * V-H2: the client ignored its own `m.call.member active=false`.
//
// All four are ordering/lifecycle problems, so they are fixed together by
// making the lifecycle explicit and serialised:
//
//     Idle → Joining → FetchingTurn → Active → Leaving → Idle
//
// and putting every server-mutating voice request (voice/join,
// voice/leave, voice/state) through ONE in-flight-at-a-time queue. A join
// issued while a leave is in flight waits for the leave's reply rather
// than racing it.
//
// TESTABILITY
// -----------
// This class performs no network I/O and owns no engine. Outbound
// requests are signals; the transport (engine) is reached through four
// std::function hooks. ServerConnection wires the signals to MatrixClient
// and the hooks to VoiceEngine; tests/test_voice_lifecycle.cpp wires both
// to recorders. It therefore needs no server, no libdatachannel and no
// audio device — which is the whole point, because none of the code this
// replaces was ever covered by a test.

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>

// One inbound m.call.* signalling event, flattened so this class can
// hold and replay it without depending on the event model or on
// nlohmann::json. `payload` is the event's `content` object, serialised;
// the dispatcher that finally consumes it parses it back. The cost is
// paid only on signalling events (a handful per join), and in exchange
// the buffer-and-replay path is a value type a test can construct.
struct CallSignal {
    QString type;                 // "m.call.invite", "m.call.candidates", …
    QString sender;               // full user id of the sender
    QString roomId;
    qint64 originServerTs = 0;    // 0 when unknown (never dropped as stale)
    QByteArray payload;           // serialised `content` JSON object
};

class VoiceSession : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,          // not in voice, nothing in flight
        Joining,       // POST voice/join issued, awaiting its reply
        FetchingTurn,  // join accepted, GET /voip/turnServer in flight
        Active,        // transport running
        Leaving,       // local teardown done, POST voice/leave in flight
    };
    Q_ENUM(State)

    explicit VoiceSession(QObject* parent = nullptr);

    State state() const { return m_state; }
    // The room this session is in, joining, or leaving. Empty in Idle.
    QString roomId() const { return m_roomId; }
    // True only in Active — i.e. an engine is running. `roomId()` is
    // non-empty in Joining/FetchingTurn/Leaving too, so callers that mean
    // "the user is in a call" must use this.
    bool isActive() const { return m_state == State::Active; }
    // True while a server round trip we issued is outstanding.
    bool busy() const { return m_inFlight.has_value(); }

    bool muted() const { return m_muted; }
    bool deafened() const { return m_deafened; }
    // The mic gate we actually apply and announce: manual mute OR
    // (push-to-talk mode AND the key is not held). V-M7 — the old code
    // announced `muted=false` to the roster while PTT held the mic shut,
    // so everyone else saw an unmuted user who was never audible.
    bool effectiveMuted() const;

    void setLocalUserId(const QString& userId) { m_localUserId = userId; }
    // The server's opaque token for our CURRENT membership, from the
    // voice/join response. Echoed on leave and on every state PUT so the
    // server can refuse a request that belongs to a membership it has
    // already replaced — the server-side half of V-H1. Empty against a
    // server that does not issue one, and every rule below degrades to
    // the old behaviour in that case.
    QString sessionId() const { return m_sessionId; }
    QString localUserId() const { return m_localUserId; }

    // ---- Transport hooks -------------------------------------------
    // startEngine returns false when the session cannot possibly work
    // (no audio device, relay-only policy with no TURN); the session then
    // unwinds the join exactly as a failed TURN fetch does.
    using EngineStarter = std::function<bool(const QString& roomId,
                                             const QJsonArray& members,
                                             const QJsonObject& turnConfig)>;
    using EngineStopper = std::function<void()>;
    // Hand one inbound signalling event to the running transport.
    using SignalSink = std::function<void(const CallSignal&)>;
    // Push refreshed TURN credentials into the running transport, so
    // peer connections built after `ttl` do not use expired creds (V-M3).
    using TurnUpdater = std::function<void(const QJsonObject&)>;
    // Apply the effective mic gate to the running transport.
    using MicGate = std::function<void(bool effectiveMuted)>;
    // Apply the deafen state to the running transport. Separate from
    // MicGate because they gate opposite directions — mute is capture,
    // deafen is playback — and, more to the point, because deafen used
    // to have no hook at all: it reached the engine only through
    // ServerConnection's deafenedChanged handler, which by definition
    // does not fire when the value has not changed. A user who was
    // already deafened and then joined (or switched channel) got a
    // freshly-built engine at the AudioEngine default, undeafened, and
    // heard everybody while the UI showed them deafened.
    using DeafenGate = std::function<void(bool deafened)>;

    void setEngineStarter(EngineStarter f) { m_startEngine = std::move(f); }
    void setEngineStopper(EngineStopper f) { m_stopEngine = std::move(f); }
    void setSignalSink(SignalSink f) { m_signalSink = std::move(f); }
    void setTurnUpdater(TurnUpdater f) { m_turnUpdater = std::move(f); }
    void setMicGate(MicGate f) { m_micGate = std::move(f); }
    void setDeafenGate(DeafenGate f) { m_deafenGate = std::move(f); }

    // ---- Intents (UI / app) ----------------------------------------
    // Join `roomId`. Switching channels enqueues the leave of the current
    // room first, so the server sees leave-then-join in that order.
    void requestJoin(const QString& roomId);
    // Leave whatever room this session holds. No-op when Idle.
    void requestLeave();
    void toggleMute();
    void toggleDeafen();
    void setPttMode(bool ptt);
    void setPttPressed(bool pressed);
    // Announce media flags (screen share / camera). Queued like any other
    // state PUT so it cannot overtake a join or a leave.
    void announceMedia(bool screenSharing, bool cameraOn);

    // ---- Replies from the server -----------------------------------
    void onJoinSucceeded(const QString& roomId, const QJsonArray& members,
                         const QString& sessionId = QString());
    void onJoinFailed(const QString& roomId, const QString& error);
    void onLeaveSucceeded(const QString& roomId);
    void onLeaveFailed(const QString& roomId, const QString& error);
    void onTurnConfig(const QJsonObject& config);
    void onTurnConfigFailed(const QString& error);
    void onStateUpdateSucceeded(const QString& roomId);
    void onStateUpdateFailed(const QString& roomId, const QString& error);
    // The server refused a state PUT because our token is stale (403).
    // The membership is gone and no state PUT can bring it back, so this
    // takes the same recovery path as a retracted self-row: re-POST the
    // join once, then tear down.
    void onStateSuperseded(const QString& roomId);
    // The polled roster for the room we are in. Used for nothing here but
    // keeping `m_members` fresh for a re-POSTed join.
    void onMembersPolled(const QString& roomId, const QJsonArray& members);
    // Our OWN m.call.member state event. V-H2: `active=false` while we
    // believe we are in the call means the server has retired us (ghost
    // reaper after sleep, a moderator, or the V-H1 race). Re-POST the
    // join exactly once; if that fails, tear down with an error.
    // `sessionId` is the token the retraction names. When it differs from
    // ours, an OLDER membership of ours was cleaned up (the server echoes
    // the retracted session on every leave/rejoin-reset/reap) and this is
    // not about the session we are running — ignoring it is what stops a
    // join-over-active-row's own active=false edge from tearing down the
    // session it just created.
    void onSelfMembership(const QString& roomId, bool active,
                          const QString& sessionId = QString());

    // ---- Inbound signalling ----------------------------------------
    // Buffers while the engine does not exist yet (V-C1) and drops
    // invites older than this session (V-M1). Everything else goes
    // straight to the sink.
    void routeInboundSignal(const CallSignal& signal);

    // Number of events currently parked for replay. Test/diagnostic.
    int bufferedSignalCount() const { return m_buffered.size(); }
    // Queue depth of pending server requests. Test/diagnostic.
    int queuedRequestCount() const { return m_queue.size(); }

    // Synchronous, unconditional local teardown WITHOUT a server round
    // trip — used when the connection itself is going away. Leaves the
    // session Idle and drops the queue.
    void abandon();

    static const char* stateName(State s);

signals:
    // Outbound requests. ServerConnection connects these to MatrixClient;
    // the test harness records them.
    void joinRequested(const QString& roomId);
    void leaveRequested(const QString& roomId, const QString& sessionId);
    void turnConfigRequested();
    void stateUpdateRequested(const QString& roomId, bool muted, bool deafened,
                              const QString& sessionId);
    void mediaStateUpdateRequested(const QString& roomId, bool screenSharing,
                                   bool cameraOn, const QString& sessionId);

    void stateChanged();
    void mutedChanged();
    void deafenedChanged();
    // A user-facing failure. Not every error unwinds the session.
    void errorOccurred(const QString& message);
    // Emitted once the session has reached Idle with an empty queue, i.e.
    // every leave we issued has been answered. The quit path waits on it.
    void settled();

private:
    enum class OpKind { Join, Rejoin, Leave, StateUpdate, MediaUpdate };
    struct Op {
        OpKind kind;
        QString roomId;
        // Snapshot of m_sessionId at the moment the op was ISSUED, so a
        // reply can be matched even after the token has moved on.
        QString sessionId;
        bool muted = false;
        bool deafened = false;
        bool screenSharing = false;
        bool cameraOn = false;
    };

    // Where the queue, once drained, leaves this session: the room we
    // will be in, or empty for "out of voice".
    QString destinationRoom() const;
    // Builds an op stamped with the CURRENT session token.
    Op makeOp(OpKind kind, const QString& roomId) const;
    // A state PUT announcing the EFFECTIVE mic gate plus the deafen
    // flag, stamped with the current token.
    Op stateOp(const QString& roomId) const;
    void enqueue(Op op);
    void pump();
    void completeOp();
    void setState(State s);
    // By value: it clears m_roomId, which callers pass in.
    void unwind(QString roomId, QString reason);
    void replayBufferedSignals();
    void applyMicGate();
    void applyDeafenGate();
    void scheduleTurnRefresh(const QJsonObject& config);
    // True when `op` is the request currently awaiting a reply and it
    // concerns `roomId`. Late replies to superseded requests are dropped.
    bool inFlightIs(OpKind kind, const QString& roomId) const;

    State m_state = State::Idle;
    QString m_roomId;
    QString m_localUserId;
    // Token for the membership we currently hold; cleared whenever the
    // session leaves Active/FetchingTurn, so a stale one is never echoed.
    QString m_sessionId;
    QJsonArray m_members;
    QJsonObject m_turnConfig;

    QQueue<Op> m_queue;
    std::optional<Op> m_inFlight;

    // Mute/deafen: `m_muted`/`m_deafened` are what the user asked for;
    // `m_ackedMuted`/`m_ackedDeafened` are what the server last accepted.
    // A failed PUT rolls the former back to the latter (V-M7).
    bool m_muted = false;
    bool m_deafened = false;
    bool m_ackedMuted = false;
    bool m_ackedDeafened = false;
    bool m_pttMode = false;
    bool m_pttPressed = false;
    // Last gate value handed to the transport, so a redundant apply is
    // free and so a reconnecting engine can be re-gated.
    std::optional<bool> m_appliedGate;
    // What the running transport was last told about deafening. Reset
    // whenever a NEW engine is about to be fed, so the state is pushed
    // again rather than suppressed as a no-op — the same reason
    // m_appliedGate is reset in onTurnConfig.
    std::optional<bool> m_appliedDeafen;

    // V-H2 — one re-POST per retirement, never a loop.
    bool m_rejoinAttempted = false;

    // V-M1 — local ms epoch at which the current join attempt started.
    // Invites stamped before it belong to a previous session.
    qint64 m_sessionStartMs = 0;

    QVector<CallSignal> m_buffered;

    QTimer* m_turnRefreshTimer = nullptr;

    EngineStarter m_startEngine;
    EngineStopper m_stopEngine;
    SignalSink m_signalSink;
    TurnUpdater m_turnUpdater;
    MicGate m_micGate;
    DeafenGate m_deafenGate;

public:
    // Bound on the V-C1 replay buffer. A join that never completes must
    // not let a hostile or broken peer grow this without limit; 256 is
    // far more signalling than a real join produces (one invite plus a
    // couple of candidate batches per peer).
    static constexpr int kMaxBufferedSignals = 256;
    // Clock-skew allowance for the V-M1 staleness test. origin_server_ts
    // is the SERVER's clock and m_sessionStartMs is ours, so a strict
    // comparison would discard perfectly good invites on a machine whose
    // clock runs a minute fast. Stale replays after a failed sync resume
    // are minutes to hours old, so a generous allowance still catches
    // them.
    static constexpr qint64 kClockSkewAllowanceMs = 60'000;
    // Fallback TURN lifetime when the server omits `ttl`, matching the
    // server's own default of 3600 s.
    static constexpr int kDefaultTurnTtlSeconds = 3600;
};
