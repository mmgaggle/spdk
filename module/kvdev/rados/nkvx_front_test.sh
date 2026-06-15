#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Intel Corporation. All rights reserved.
#
# Slice C4 front-client core loopback: boot the Slice C2 executor (nkvx_service),
# then forward one nkvx_exec through the FRONT client core (nkvx_front_client.c),
# driving nkvx_front_progress() exactly as the integrated SPDK poller will, and
# assert the async completion fires with the C2 skeleton status (NOT_SUPPORTED).
# This proves the origin-side forward/progress/completion mechanism end to end
# over a real transport with no SPDK reactor.
#
# Default transport na+sm://; pass a provider as $1 (e.g. "ofi+tcp://127.0.0.1").
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_ROOT="$(cd "$HERE/../../.." && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-na+sm://}"
SVC_DIR="$HERE/nkvx_service"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

# Build both sides if needed.
make -C "$SVC_DIR" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || {
	echo "FAIL: could not build the C2 executor"; exit 1; }
make -C "$HERE" -f Makefile.front.ut MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || {
	echo "FAIL: could not build the front client test"; exit 1; }

ADDR_FILE="$(mktemp -u /tmp/nkvx-front-addr.XXXXXX)"
SVC_LOG="$(mktemp /tmp/nkvx-front-svc.XXXXXX)"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
		kill "$SVC_PID" 2>/dev/null
		wait "$SVC_PID" 2>/dev/null
	fi
	rm -f "$ADDR_FILE" "$SVC_LOG"
}
trap cleanup EXIT

echo "== C4 front-client loopback (transport=$TRANSPORT) =="

"$SVC_DIR/nkvx_service" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" >"$SVC_LOG" 2>&1 &
SVC_PID=$!

for _ in $(seq 1 50); do
	[ -s "$ADDR_FILE" ] && break
	if ! kill -0 "$SVC_PID" 2>/dev/null; then
		echo "FAIL: executor exited before publishing an address"
		cat "$SVC_LOG"; exit 1
	fi
	sleep 0.1
done
if [ ! -s "$ADDR_FILE" ]; then
	echo "FAIL: timed out waiting for the executor address"
	cat "$SVC_LOG"; exit 1
fi
echo "executor addr: $(cat "$ADDR_FILE")"

"$HERE/nkvx_front_client_test" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" --expect -7
RC=$?

echo "--- executor log ---"; cat "$SVC_LOG"

if [ "$RC" -ne 0 ]; then
	echo "RESULT: FAIL (front test rc=$RC)"
	exit 1
fi
echo "RESULT: PASS"
exit 0
