#pragma once

#include <QtGlobal>

// Tells macOS that this process is doing latency-critical, user-visible
// work — presenting a video stream — so that it does not coalesce or
// defer our timers the way it does for a process it believes is idle.
//
// Why: the playout buffer (VideoPlayoutBuffer.h) releases pictures from
// a Qt::PreciseTimer on the GUI thread, 30-60 wake-ups a second. XNU
// coalesces timer wake-ups for processes it considers background — App
// Nap for an app whose windows are hidden or occluded, a latency QoS
// tier or PRIO_DARWIN_BG for a daemon's children — and a coalesced timer
// fires on a grid of tens to a hundred milliseconds. Measured on this
// codebase: under `taskpolicy -b` the presentation timer fired every
// ~104 ms and the clump test showed 5 of 11 frames. No buffer design can
// show 30 fps from a 10 Hz timer.
//
// NSActivityLatencyCritical is the documented opt-out for exactly this
// ("affects timer coalescing"); NSActivityUserInitiated keeps App Nap off
// while a stream is on screen. "AllowingIdleSystemSleep": we need timers,
// not to hold the Mac awake — if the machine idles into sleep nobody is
// watching, and a video player that blocks sleep is a battery complaint.
//
// What it cannot do: override a policy imposed from outside. Under
// `taskpolicy -b` the timer still fired every ~104 ms with this activity
// held (measured) — PRIO_DARWIN_BG outranks it. That is a CI runner's
// problem, not the owner's; for the app it is the system's own idle
// heuristics (App Nap, timer coalescing of an unfocused app) that matter,
// and those are what this opts out of.
//
// Held by VideoReceivePipeline for as long as it is presenting through
// the playout buffer (from its first held frame until smoothing is turned
// off or the pipeline is retired when the stream stops). One per active
// stream; the OS reference-counts them. A no-op off macOS, where nothing
// throttles a foreground process's timers this way.
class LatencyCriticalActivity {
public:
    LatencyCriticalActivity() = default;
    ~LatencyCriticalActivity() { end(); }
    LatencyCriticalActivity(const LatencyCriticalActivity&) = delete;
    LatencyCriticalActivity& operator=(const LatencyCriticalActivity&) = delete;

#if defined(Q_OS_MACOS)
    void begin();   // idempotent
    void end();     // idempotent
#else
    void begin() {}
    void end() {}
#endif
    bool active() const { return m_token != nullptr; }

private:
    void* m_token = nullptr;   // the NSActivity token, retained
};
