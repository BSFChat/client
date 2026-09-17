#!/usr/bin/env bash
#
# Stands up a real coturn, runs test_relay_e2e against it, tears it down.
#
# Opt-in. It needs Docker, it binds UDP ports on the host, and it takes a few
# seconds — none of which belongs in the sweep everyone runs before a commit.
# Configure with -DBSFCHAT_RELAY_E2E=ON to register it with ctest, or just run
# this script with the test binary's path in BSFCHAT_RELAY_TEST_BIN.
#
# What it proves is in the header of tests/test_relay_e2e.cpp. The short
# version: that rtc::TransportPolicy::Relay really does stop this machine's LAN
# and public addresses reaching the wire, which is a claim no unit test can make
# because there is no gatherer in one.
#
# coturn notes, both of which bit on the way here:
#   * --allow-loopback-peers is REQUIRED. coturn denies relaying to private and
#     loopback ranges by default, which is correct for a deployment and fatal
#     for two peers on one machine. It is also exactly the reason the brief says
#     relay cannot work between two hosts on one LAN against the production
#     coturn — that denial is the same rule.
#   * --external-ip must be an address the host can actually send to. Docker
#     Desktop does not share the host's network namespace, so the container sees
#     its own address and would otherwise hand out an unroutable relay endpoint.

set -euo pipefail

CONTAINER=bsfchat-relay-e2e
IMAGE=coturn/coturn:latest
TURN_PORT=${BSFCHAT_TURN_PORT:-3478}
RELAY_LO=${BSFCHAT_RELAY_PORT_LO:-49160}
RELAY_HI=${BSFCHAT_RELAY_PORT_HI:-49200}
TURN_USER=e2e
TURN_PASS=e2e

TEST_BIN=${BSFCHAT_RELAY_TEST_BIN:-}
if [[ -z "$TEST_BIN" ]]; then
    echo "BSFCHAT_RELAY_TEST_BIN is unset (path to test_relay_e2e)" >&2
    exit 2
fi

if ! docker info >/dev/null 2>&1; then
    echo "Docker is not running — this test needs it to start coturn." >&2
    exit 2
fi

# The address the container should tell clients to reach its relay on. Must be
# reachable FROM the host, so it is the host's own LAN address rather than
# anything the container can see about itself.
host_ip() {
    if [[ -n "${BSFCHAT_HOST_IP:-}" ]]; then echo "$BSFCHAT_HOST_IP"; return; fi
    if command -v ipconfig >/dev/null 2>&1; then          # macOS
        for dev in en0 en1 en2; do
            ip=$(ipconfig getifaddr "$dev" 2>/dev/null || true)
            [[ -n "$ip" ]] && { echo "$ip"; return; }
        done
    fi
    if command -v hostname >/dev/null 2>&1; then          # Linux
        ip=$(hostname -I 2>/dev/null | awk '{print $1}')
        [[ -n "$ip" ]] && { echo "$ip"; return; }
    fi
    echo ""
}

HOST_IP=$(host_ip)
if [[ -z "$HOST_IP" ]]; then
    echo "Could not determine a host IP for --external-ip; set BSFCHAT_HOST_IP." >&2
    exit 2
fi

cleanup() { docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

echo "Starting coturn (external-ip=$HOST_IP, relay ports $RELAY_LO-$RELAY_HI)…"
docker run -d --name "$CONTAINER" \
    -p "${TURN_PORT}:${TURN_PORT}/udp" \
    -p "${RELAY_LO}-${RELAY_HI}:${RELAY_LO}-${RELAY_HI}/udp" \
    "$IMAGE" -n \
    --listening-port="${TURN_PORT}" \
    --listening-ip=0.0.0.0 \
    --external-ip="${HOST_IP}" \
    --relay-ip=0.0.0.0 \
    --min-port="${RELAY_LO}" --max-port="${RELAY_HI}" \
    --realm=bsfchat.test \
    --user="${TURN_USER}:${TURN_PASS}" \
    --lt-cred-mech --fingerprint \
    --allow-loopback-peers \
    --allowed-peer-ip=10.0.0.0-10.255.255.255 \
    --allowed-peer-ip=172.16.0.0-172.31.255.255 \
    --allowed-peer-ip=192.168.0.0-192.168.255.255 \
    --no-tls --no-cli >/dev/null

# Give it a moment to bind, and fail early with its own log if it did not.
sleep 3
if [[ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null)" != "true" ]]; then
    echo "coturn exited during startup:" >&2
    docker logs "$CONTAINER" 2>&1 | tail -30 >&2
    exit 1
fi

export BSFCHAT_TURN_URI="turn:${HOST_IP}:${TURN_PORT}?transport=udp"
export BSFCHAT_TURN_USER="$TURN_USER"
export BSFCHAT_TURN_PASS="$TURN_PASS"

# NOT set here. With coturn on this machine the peer address in every
# CREATE_PERMISSION is coturn's own external address, and coturn refuses to
# relay to itself — 403, correctly. The gathering assertions (the privacy claim)
# run regardless; the media half needs a TURN server genuinely elsewhere. Export
# BSFCHAT_RELAY_MEDIA_CHECK=1 with BSFCHAT_TURN_URI pointing at one, and skip
# this script's container entirely.
: "${BSFCHAT_RELAY_MEDIA_CHECK:=}"

echo "Running $TEST_BIN against $BSFCHAT_TURN_URI"
set +e
"$TEST_BIN"
rc=$?
set -e

if [[ $rc -ne 0 ]]; then
    echo "--- coturn log ---" >&2
    docker logs "$CONTAINER" 2>&1 | tail -40 >&2
fi
exit $rc
