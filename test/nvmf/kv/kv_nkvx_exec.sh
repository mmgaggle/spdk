#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# rados-nkvx TB1 end-to-end (ADR-0009): a CPU-issued NVMe-KV Exec runs a built-in
# WASM-seam module in the NEW in-process sandboxed executor, OFF the SPDK reactor,
# against an object cold-filled from RADOS via librados. This is NOT the legacy
# cls/rados_read_op_exec path (that one is kv_rados_exec.sh).
#
# Wires the librados-backed kvdev to a KV namespace, allowlists:
#   op_id 10 -> "nkvx:bytecount"       (result = object length as LE u64; built-in)
#   op_id 11 -> "nkvx:identity"        (result = object bytes copied back; built-in)
#   op_id 12 -> "nkvx:wasm:bytecount"  (result = object length as LE u64; REAL .wasm)
# then runs kv_host nkvx-exec, which Stores a value and issues those Execs and a
# concurrent-Retrieve latency probe proving the reactor is not blocked.
#
# The "nkvx:" binding prefix routes KV Exec to the executor; the text after it
# names the module. A "wasm:<name>" module runs a REAL precompiled <name>.wasm in
# the dlopen-backed wasmtime runtime (ADR-0013) instead of a C built-in; it
# requires an SPDK built --with-wasm, libwasmtime.so installed (see below), and
# SPDK_NKVX_WASM_DIR pointing at the .wasm directory.
#
# Off-reactor proof is DETERMINISTIC (not timing-based): the target logs the
# reactor OS thread id and the worker OS thread id the module actually ran on for
# every Exec ("nkvx: off-reactor proof ... off_reactor=YES"). This script greps
# the target log and asserts those ids differ. No artificial delay is used.
#
# Requires: a vstart Ceph cluster UP with pool 'kvpool', and an SPDK build
# configured --with-rbd --with-vfio-user (and --with-wasm for the op_id 12 real-
# wasm Exec). No OSD-side object class is needed (that's the whole point of
# ADR-0009). libwasmtime.so must be dlopen'able by the target: install it to
# /usr/local/lib (the executor probes that path) or put it on LD_LIBRARY_PATH.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

: "${CEPH_CONF:=/mnt/ceph/build/ceph.conf}"
export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="nkvx_cluster"
kvdev_name="KvNkvx0"
nqn="nqn.2026-06.io.spdk:kv-nkvx-cnode0"
nsid=1

sock_dir=$(mktemp -d /tmp/kv_nkvx_exec.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

# Fail-fast OSD/pool liveness precheck: the executor cold-fills via librados, so
# a dead/wedged OSD would otherwise hang. Bounded probe BEFORE bringing up target.
OSD_PRECHECK_TIMEOUT_S="${OSD_PRECHECK_TIMEOUT_S:-15}"
rados_bin="$(dirname "$CEPH_CONF")/bin/rados"
[[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout "$OSD_PRECHECK_TIMEOUT_S" "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls > /dev/null 2>&1; then
	echo "kv_nkvx_exec: FAIL (OSD/pool '$pool_name' not reachable within ${OSD_PRECHECK_TIMEOUT_S}s; is the vstart OSD up?)"
	exit 1
fi
echo "OSD liveness precheck: pool '$pool_name' reachable"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

# Build the host app if needed.
make -C "$testdir" > /dev/null

# Directory holding precompiled <name>.wasm modules for the real-wasm Exec path
# (op_id 12). The executor reads SPDK_NKVX_WASM_DIR to locate "bytecount.wasm".
export SPDK_NKVX_WASM_DIR="$testdir/wasm"

# Start the target. Capture its log so we can grep the deterministic off-reactor
# proof (reactor_tid vs worker run_tid) the executor emits for each Exec. The
# target inherits SPDK_NKVX_WASM_DIR so the dlopen-backed wasmtime runtime can
# find bytecount.wasm.
tgt_log="$sock_dir/nvmf_tgt.log"
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 > "$tgt_log" 2>&1 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

# Wire up: VFIOUSER transport + rados cluster/kvdev + subsystem + KV ns + listener.
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKNKVX01 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Allowlist the nkvx op-IDs. Binding format is "nkvx:<module>"; the CLI splits
# each token on the FIRST ':' into op_id and binding, so "10:nkvx:bytecount"
# yields op_id=10 binding="nkvx:bytecount", and "12:nkvx:wasm:bytecount" yields
# op_id=12 binding="nkvx:wasm:bytecount" (module "wasm:bytecount" -> real .wasm).
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "10:nkvx:bytecount 11:nkvx:identity 12:nkvx:wasm:bytecount"
get_json=$($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")
echo "nvmf_ns_get_kv_exec_allowlist => $get_json"

# Run the host: Store + KV Exec(bytecount) + KV Exec(identity) + off-reactor
# latency probe. The module executes in-process OFF the reactor; the host asserts
# the returned bytes and the concurrent-Retrieve latency margin.
host_log="$sock_dir/kv_host.log"
"$testdir/kv_host" "$muser_dir" nkvx-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]] && ! grep -q "PASS: KV Exec nkvx e2e (off-reactor built-in module ran)" "$host_log"; then
	echo "kv_nkvx_exec: FAIL (nkvx KV Exec e2e did not report PASS)"
	rc=1
fi

# Criterion 2: DETERMINISTIC off-reactor proof. The target logs, per Exec, the
# reactor OS thread id and the worker OS thread id the module body ran on. Assert
# at least one such line exists AND that every proof line reports off_reactor=YES
# (worker tid != reactor tid). Any off_reactor=NO would mean the compute ran on
# the polled reactor -> hard fail.
if [[ $rc -eq 0 ]]; then
	proof_lines=$(grep -c "nkvx: off-reactor proof" "$tgt_log" || true)
	bad_lines=$(grep "nkvx: off-reactor proof" "$tgt_log" | grep -c "off_reactor=NO" || true)
	if [[ "${proof_lines:-0}" -lt 1 ]]; then
		echo "kv_nkvx_exec: FAIL (no off-reactor proof lines in target log)"
		rc=1
	elif [[ "${bad_lines:-0}" -ne 0 ]]; then
		echo "kv_nkvx_exec: FAIL ($bad_lines Exec(s) ran ON the reactor — off_reactor=NO)"
		grep "nkvx: off-reactor proof" "$tgt_log" | grep "off_reactor=NO" || true
		rc=1
	else
		echo "Off-reactor proof: $proof_lines Exec(s), all off_reactor=YES (worker tid != reactor tid)"
		grep "nkvx: off-reactor proof" "$tgt_log" | head -2 || true
	fi
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_rados_delete "$kvdev_name"
$rpc_py kvdev_rados_unregister_cluster "$cluster_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_nkvx_exec: PASS"
else
	echo "kv_nkvx_exec: FAIL (rc=$rc)"
fi
exit $rc
