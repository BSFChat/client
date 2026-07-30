#pragma once

#include <QString>

// Retry-schedule maths for SyncLoop, kept as free functions with no Qt
// event-loop or network state so the schedule is directly unit-testable
// (see tests/test_backoff.cpp) rather than only observable by watching a
// live client fail to reach a server.
//
// The loop this replaced retried on a flat 5s single-shot timer with no
// cap, no jitter and no ceiling, and re-entered the request immediately on
// success. That gave a downed server a fleet of clients dialling it every
// 5s in lockstep forever, and turned any endpoint that answered 200
// instantly — a caching proxy, a misconfigured reverse proxy, a server bug
// — into a tight request loop.
namespace SyncBackoff {

// First retry delay, and the ceiling the schedule saturates at.
constexpr int kBaseDelayMs = 1000;
constexpr int kMaxDelayMs = 60000;

// Minimum wall-clock gap between the *starts* of two successive successful
// syncs. /sync is a long poll (30s), so in healthy operation nothing ever
// comes near this floor; it only bites when the far end answers without
// blocking.
constexpr int kMinSyncIntervalMs = 250;

// How many consecutive failures a resumed `since` token is given before it
// is abandoned in favour of a full initial sync, regardless of what the
// error said. The backstop matters more than the errcode sniffing below:
// an unrecognised error shape must still end in a working client.
constexpr int kMaxResumeAttempts = 3;

// Deterministic delay for the Nth consecutive failure, 0-based:
// 1s, 2s, 4s, 8s, 16s, 32s, then flat at kMaxDelayMs.
int baseDelayMs(int consecutiveFailures);

// "Equal jitter": half the delay stays deterministic, the other half is
// spread over `jitter01` ∈ [0, 1]. Keeping a floor of base/2 means jitter
// can never make a client retry *faster* than the schedule intends, while
// still breaking the lockstep that makes a whole fleet hit a recovering
// server in the same millisecond.
int applyJitter(int baseDelayMs, double jitter01);

// baseDelayMs() + applyJitter() in one call.
int delayForFailure(int consecutiveFailures, double jitter01);

// True when a /sync error body is the server saying it does not recognise
// our `since` token — the only class of failure that dropping the token and
// re-running a full initial sync can actually fix.
//
// Matches on the parsed `errcode`, not a substring: "M_UNKNOWN_TOKEN"
// contains "M_UNKNOWN" but means the *access* token is dead, and throwing
// away the sync position for it would hide a needed re-login behind an
// expensive full sync that fails identically.
bool indicatesRejectedSinceToken(const QString& errorBody);

} // namespace SyncBackoff
