#pragma once

// Waiting for voice leaves to actually reach the wire at shutdown.
//
// V-H3: `aboutToQuit` called `leaveAllVoice()`, which queued an HTTP POST
// on QNetworkAccessManager — and then the process exited before the
// socket was written. Nothing left the machine. The peers got no hangup
// and the server carried the row until its ghost reaper caught up, so
// every quit left a phantom in the channel for 30–40 s.
//
// The fix is a BOUNDED nested event loop: keep pumping until every
// session reports it has settled (its leave was answered), or the
// deadline passes. Bounded because a hung server must not stop the user
// from quitting — half a second is long enough for a LAN round trip and
// short enough that nobody notices it.

#include "net/VoiceSession.h"

#include <QEventLoop>
#include <QList>
#include <QPointer>
#include <QTimer>

namespace voice {

// Pumps the event loop until every session in `sessions` is Idle with an
// empty request queue, or `timeoutMs` elapses. Returns true if they all
// settled in time. Sessions already settled cost nothing, and an empty
// list returns true immediately without entering a loop at all.
inline bool waitForSessionsToSettle(const QList<VoiceSession*>& sessions,
                                    int timeoutMs)
{
    QList<QPointer<VoiceSession>> pending;
    for (auto* s : sessions) {
        if (!s) continue;
        // Settled means: nothing in flight AND nothing queued behind it.
        if (s->state() == VoiceSession::State::Idle && !s->busy()
            && s->queuedRequestCount() == 0) {
            continue;
        }
        pending.append(s);
    }
    if (pending.isEmpty()) return true;

    QEventLoop loop;
    int outstanding = int(pending.size());
    for (const auto& s : pending) {
        QObject::connect(s, &VoiceSession::settled, &loop, [&]() {
            if (--outstanding <= 0) loop.quit();
        });
        // A connection torn down mid-wait must not hold the loop open.
        QObject::connect(s, &QObject::destroyed, &loop, [&]() {
            if (--outstanding <= 0) loop.quit();
        });
    }

    bool timedOut = false;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    guard.start(timeoutMs);

    loop.exec();
    return !timedOut && outstanding <= 0;
}

} // namespace voice
