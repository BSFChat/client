# Close the DTLS-fingerprint window in libdatachannel's
# PeerConnection::setRemoteDescription (present in v0.24.5 and still on
# upstream master as of 2026-09).
#
# THE BUG. setRemoteDescription() ends with these two calls, in this order:
#
#     iceTransport->setRemoteDescription(description);
#     impl()->processRemoteDescription(std::move(description));
#
# The first hands libjuice the remote ICE ufrag/pwd; from that instant the
# agent's own thread can validate the connectivity checks the peer has
# already been sending and declare ICE Connected. That fires
# initDtlsTransport() and starts the DTLS handshake — on the ICE thread,
# while THIS thread has not yet reached the second call, which is the one
# that stores mRemoteDescription. When the peer's certificate arrives in
# that window, PeerConnection::checkFingerprint() finds no remote
# description at all and returns false before it ever compares anything:
#
#     if (!mRemoteDescription || !mRemoteDescription->fingerprint() || ...)
#         return false;
#
# OpenSSL turns that into "certificate verify failed" locally and a
# "tlsv1 alert unknown ca" at the peer. The connection is dead; nothing
# retries, because as far as both ends are concerned the other side
# presented a certificate that did not match the SDP.
#
# WHY WE SEE IT AND MOST USERS DO NOT. The window is the wall-clock gap
# between the two statements — normally microseconds. It only matters when
# the peer can complete ICE and get a certificate on the wire inside it,
# which needs a sub-millisecond path: two peers on ONE host over 127.0.0.1.
# That is exactly our loopback tests (test_voice_teardown,
# test_media_loopback, test_video_rtp_loopback), and exactly the
# two-clients-on-one-machine harness PLAN-2026-09.md asks for. Over a real
# network the RTT closes the window long before the handshake gets there.
# It surfaced first in the ASan+UBSan CI job because -O1 + sanitizers +
# `ctest -j4` on a 3-core hosted runner is what makes this thread lose the
# CPU right there. Incident: client CI run 35523785278, "asan+ubsan
# (macos)", test_voice_teardown, "FAIL: data channels never opened".
#
# MEASURED, on an M-series Mac, ASan+UBSan build, by injecting a sleep
# between those two statements and running test_voice_teardown five times
# per step (each run brings a pair up four times):
#
#     stall injected   0ms   1ms   2ms   3ms   5ms  30ms  120ms
#     before the fix   0/5   0/5   4/5   3/5   5/5   5/5    5/5
#     after  the fix   0/5    -    0/5    -    0/5   0/5    0/5
#
# So ~2 ms of lost CPU on this thread is the whole bug, and the failure it
# produces is byte-for-byte the one CI reported. Unassisted it never
# reproduced here: 1,120 bring-ups (160 serial runs plus 960 under four
# concurrent copies) were all green, which is why this needs the injection
# to be demonstrated at all — and why it reads as a flake until you look
# at the fact that it is a CERTIFICATE rejection rather than a timeout.
#
# THE FIX is the ordering: store the remote description (and therefore its
# fingerprint) BEFORE letting ICE proceed. Then there is no window — the
# handshake cannot start before the thing it is checked against exists.
#
# Deliberate consequence: iceTransport->setRemoteDescription() can still
# throw (illegal actpass in an answer, incompatible roles, ICE settings
# libjuice refuses), and it now throws with the remote description already
# stored and its reciprocated tracks already created, where before it threw
# with nothing applied. Signaling state is still not advanced, and every
# caller in src/voice/PeerConnectionManager.cpp routes that throw into
# failPeer(), which tears the peer down and lets the reconciler re-offer —
# so the half-applied state is discarded immediately. We took that over the
# alternative (duplicating libjuice's role negotiation on our side to
# validate first), which would rot against the pin.
#
# Upstream has not been told yet — see the report for this branch. If a
# later pin fixes it, this file's anchors will stop matching and the build
# will fail loudly rather than silently leaving the race in.
#
# Idempotent: safe to re-run over an already-patched tree (FetchContent
# re-runs PATCH_COMMAND whenever it re-populates).

set(_pc "src/peerconnection.cpp")

if(NOT EXISTS "${_pc}")
    message(FATAL_ERROR
        "patch-libdatachannel: ${_pc} not found in ${CMAKE_CURRENT_SOURCE_DIR}. "
        "The libdatachannel layout changed; re-check the setRemoteDescription "
        "ordering fix before moving the pin.")
endif()

file(READ "${_pc}" _content)

set(_marker "BSFCHAT: remote description before ICE credentials")

if(_content MATCHES "${_marker}")
    return()  # already patched
endif()

set(_before
"\ticeTransport->setRemoteDescription(description); // ICE transport might reject the description

\timpl()->processRemoteDescription(std::move(description));")

set(_after
"\t// ${_marker}.
\t// juice_set_remote_description() below lets the ICE thread finish its
\t// checks and start DTLS at once; on loopback it beats this thread to
\t// checkFingerprint(), which rejects the peer certificate because
\t// mRemoteDescription is not stored yet. Store it first. See
\t// cmake/patch-libdatachannel.cmake for the full incident.
\timpl()->processRemoteDescription(description);

\ticeTransport->setRemoteDescription(description); // ICE transport might reject the description")

string(FIND "${_content}" "${_before}" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR
        "patch-libdatachannel: could not find the setRemoteDescription "
        "ordering anchor in ${_pc}. The libdatachannel pin moved. Check "
        "whether upstream fixed the DTLS fingerprint race (the two calls "
        "iceTransport->setRemoteDescription / processRemoteDescription must "
        "be in that second order); if it did, delete this patch and its "
        "PATCH_COMMAND in cmake/Dependencies.cmake.")
endif()

string(REPLACE "${_before}" "${_after}" _content "${_content}")
file(WRITE "${_pc}" "${_content}")
message(STATUS "Patched libdatachannel: remote description stored before ICE credentials")
