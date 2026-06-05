#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# kv_offload_provision.sh -- bring up a co-located SPDK NVMe-oF target that
# exposes per-tenant librados KV namespaces over vfio-user.
#
# This is the data-plane bring-up an initContainer/sidecar runs so a co-located
# vLLM host (or any NVMe-KV host) can attach a KV-cache offload namespace on the
# same node. It implements the tenancy mapping of ADR-0006:
#
#   one NVMe-oF subsystem  -> one rados pool (the whole offload tier)
#   one NVMe KV namespace  -> one rados namespace per tenant tuple (nsid 1..N)
#
# For EACH tenant we create a librados kvdev bound to that tenant's rados
# namespace, then bind the kvdev to a KV namespace under the single subsystem.
# The nsid assigned (1, 2, ... in add order) is the handle the host attaches to.
#
# Prerequisites in production (ADR-0007 -- see README.md):
#   * hugepages mounted and reserved (or pass --no-huge for an unprivileged run)
#   * /dev/vfio access + IPC_LOCK for the vfio-user data path
#   * a reachable Ceph cluster (ceph.conf + keyring + cephx user) and a pool
#
# The script is idempotent-ish: re-running against an existing RPC socket reuses
# the running target, and create steps that already exist are tolerated. A trap
# tears down anything THIS invocation started.

set -euo pipefail

prog=$(basename "$0")

usage() {
	cat << EOF
Usage: $prog [options] TENANT:RADOS_NS [TENANT:RADOS_NS ...]

Bring up a co-located SPDK NVMe-oF target exposing one librados KV namespace per
tenant over vfio-user (ADR-0006). Each positional argument maps a logical tenant
name to a rados namespace; they become NVMe KV namespaces nsid 1..N (in order)
under a single subsystem backed by one rados pool.

Required Ceph / pool options:
  --ceph-conf PATH      ceph.conf path           (env CEPH_CONF)
  --keyring PATH        cephx keyring path        (env CEPH_KEYRING)
  --ceph-user USER      cephx user, e.g. admin    (env CEPH_USER, default admin)
  --pool NAME           rados pool for the tier   (env KV_POOL, default kvpool)

Topology / runtime options:
  --rpc-sock PATH       SPDK JSON-RPC unix socket (default: \$DOMAIN_DIR/rpc.sock)
  --domain-dir PATH     vfio-user domain directory (the muser endpoint dir);
                        the host attaches to this path     (default: mktemp dir)
  --nqn NQN             subsystem NQN     (default nqn.2026-06.io.spdk:kv-offload0)
  --serial SN           subsystem serial  (default SPDKKVOFF1)
  --cpumask MASK        nvmf_tgt core mask (default 0x3)
  --rados-bin PATH      rados CLI (only used by --selftest; default: rados)

Hugepages / privilege:
  --no-huge             pass --no-huge -s <MB> to nvmf_tgt for an unprivileged
                        run (no hugepages, no /dev/vfio mapping needed for the
                        target process itself). Production should NOT use this;
                        see README.md for the hugepages path.
  -s, --mem-size MB     env memory size in MB for --no-huge   (default 1024)

Lifecycle:
  --keep                do NOT tear down on exit (leave the target running so a
                        host can attach); only resources started by THIS run are
                        torn down otherwise.
  --teardown-existing   if connecting to a running target, also delete the
                        subsystem on exit (default: leave a pre-existing target
                        untouched).
  -h, --help            this help

Examples:
  # Two tenants over a live local Ceph, unprivileged (no hugepages):
  $prog --ceph-conf /etc/ceph/ceph.conf --keyring /etc/ceph/keyring \\
        --ceph-user admin --pool kvpool --no-huge -s 1024 --keep \\
        modelA:kvns modelB:kvns_t2
EOF
}

# ---- defaults ------------------------------------------------------------
CEPH_CONF="${CEPH_CONF:-}"
CEPH_KEYRING="${CEPH_KEYRING:-}"
CEPH_USER="${CEPH_USER:-admin}"
KV_POOL="${KV_POOL:-kvpool}"
RPC_SOCK=""
DOMAIN_DIR=""
NQN="nqn.2026-06.io.spdk:kv-offload0"
SERIAL="SPDKKVOFF1"
CPUMASK="0x3"
RADOS_BIN="${RADOS_BIN:-rados}"
NO_HUGE=0
MEM_SIZE=1024
KEEP=0
TEARDOWN_EXISTING=0
CLUSTER_NAME="ceph0"

tenants=()

# ---- arg parse -----------------------------------------------------------
while [[ $# -gt 0 ]]; do
	case "$1" in
		--ceph-conf)
			CEPH_CONF="$2"
			shift 2
			;;
		--keyring)
			CEPH_KEYRING="$2"
			shift 2
			;;
		--ceph-user)
			CEPH_USER="$2"
			shift 2
			;;
		--pool)
			KV_POOL="$2"
			shift 2
			;;
		--rpc-sock)
			RPC_SOCK="$2"
			shift 2
			;;
		--domain-dir)
			DOMAIN_DIR="$2"
			shift 2
			;;
		--nqn)
			NQN="$2"
			shift 2
			;;
		--serial)
			SERIAL="$2"
			shift 2
			;;
		--cpumask)
			CPUMASK="$2"
			shift 2
			;;
		--rados-bin)
			RADOS_BIN="$2"
			shift 2
			;;
		--no-huge)
			NO_HUGE=1
			shift
			;;
		-s | --mem-size)
			MEM_SIZE="$2"
			shift 2
			;;
		--keep)
			KEEP=1
			shift
			;;
		--teardown-existing)
			TEARDOWN_EXISTING=1
			shift
			;;
		-h | --help)
			usage
			exit 0
			;;
		--)
			shift
			while [[ $# -gt 0 ]]; do
				tenants+=("$1")
				shift
			done
			;;
		-*)
			echo "$prog: unknown option: $1" >&2
			usage >&2
			exit 2
			;;
		*)
			tenants+=("$1")
			shift
			;;
	esac
done

die() {
	echo "$prog: $*" >&2
	exit 1
}

[[ ${#tenants[@]} -ge 1 ]] || {
	echo "$prog: at least one TENANT:RADOS_NS required" >&2
	usage >&2
	exit 2
}
[[ -n $CEPH_CONF ]] || die "missing --ceph-conf (or CEPH_CONF)"
[[ -n $CEPH_KEYRING ]] || die "missing --keyring (or CEPH_KEYRING)"
[[ -f $CEPH_CONF ]] || die "ceph conf not found: $CEPH_CONF"
[[ -f $CEPH_KEYRING ]] || die "keyring not found: $CEPH_KEYRING"

# Resolve the SPDK tree this script lives under, to locate rpc.py and nvmf_tgt.
# Allow override so a sidecar image can point at an installed prefix.
rootdir="${SPDK_ROOT_DIR:-$(readlink -f "$(dirname "$0")/../..")}"
NVMF_TGT="${NVMF_TGT:-$rootdir/build/bin/nvmf_tgt}"
RPC_PY_BIN="${RPC_PY:-$rootdir/scripts/rpc.py}"
[[ -x $NVMF_TGT ]] || die "nvmf_tgt not found/executable: $NVMF_TGT (set NVMF_TGT or SPDK_ROOT_DIR)"
[[ -f $RPC_PY_BIN ]] || die "rpc.py not found: $RPC_PY_BIN (set RPC_PY or SPDK_ROOT_DIR)"

# Working dirs. If no domain dir was given, mint one we own and clean up.
owns_domain_dir=0
if [[ -z $DOMAIN_DIR ]]; then
	DOMAIN_DIR=$(mktemp -d /tmp/kv_offload.XXXXXX)
	owns_domain_dir=1
fi
mkdir -p "$DOMAIN_DIR"
[[ -n $RPC_SOCK ]] || RPC_SOCK="$DOMAIN_DIR/rpc.sock"

# The vfio-user listener creates "$muser_dir/cntrl"; the host attaches to the
# directory that contains it. One endpoint dir per subsystem is sufficient.
muser_dir="$DOMAIN_DIR/domain/muser0/0"
mkdir -p "$muser_dir"

rpc() { "$RPC_PY_BIN" -s "$RPC_SOCK" "$@"; }

# Return the nsid bound to a given kvdev under a subsystem, by reading
# nvmf_get_subsystems. Prefer jq when present; fall back to a python parse so
# the script works without jq installed. Prints the nsid (or nothing).
kvdev_nsid() {
	local nqn="$1" kvdev="$2"
	if command -v jq > /dev/null 2>&1; then
		rpc nvmf_get_subsystems 2> /dev/null | jq -r \
			--arg nqn "$nqn" --arg kv "$kvdev" \
			'.[] | select(.nqn==$nqn) | .namespaces[]?
			 | select(.kvdev_name==$kv) | .nsid' | head -n1
	else
		rpc nvmf_get_subsystems 2> /dev/null | python3 -c '
import json,sys
nqn,kv=sys.argv[1],sys.argv[2]
for s in json.load(sys.stdin):
    if s.get("nqn")==nqn:
        for ns in s.get("namespaces",[]):
            if ns.get("kvdev_name")==kv:
                print(ns.get("nsid")); break
' "$nqn" "$kvdev"
	fi
}

# ---- lifecycle / teardown ------------------------------------------------
nvmfpid=""
started_target=0
created_subsystem=0

cleanup() {
	local rc=$?
	if [[ $KEEP -eq 1 ]]; then
		# Leave everything up for a host to attach. Still report where.
		return $rc
	fi
	# Remove the subsystem if WE created it, or if asked to teardown an existing one.
	if [[ $created_subsystem -eq 1 || $TEARDOWN_EXISTING -eq 1 ]]; then
		rpc nvmf_subsystem_remove_listener "$NQN" -t VFIOUSER -a "$muser_dir" -s 0 2> /dev/null || true
		rpc nvmf_delete_subsystem "$NQN" 2> /dev/null || true
	fi
	# Kill the target only if WE started it.
	if [[ $started_target -eq 1 && -n $nvmfpid ]]; then
		kill "$nvmfpid" 2> /dev/null || true
		wait "$nvmfpid" 2> /dev/null || true
	fi
	if [[ $owns_domain_dir -eq 1 ]]; then
		rm -rf "$DOMAIN_DIR" 2> /dev/null || true
	fi
	return $rc
}
trap cleanup EXIT INT TERM

# ---- target bring-up -----------------------------------------------------
target_alive() {
	[[ -S $RPC_SOCK ]] && rpc rpc_get_methods > /dev/null 2>&1
}

start_or_connect_target() {
	if target_alive; then
		echo "### connecting to existing target on $RPC_SOCK"
		return 0
	fi
	local args=(-r "$RPC_SOCK" -m "$CPUMASK")
	if [[ $NO_HUGE -eq 1 ]]; then
		args+=(--no-huge -s "$MEM_SIZE")
	fi
	echo "### starting nvmf_tgt: $NVMF_TGT ${args[*]}"
	"$NVMF_TGT" "${args[@]}" &
	nvmfpid=$!
	started_target=1
	local i
	for i in $(seq 1 80); do
		if target_alive; then
			return 0
		fi
		# bail early if the target died
		if ! kill -0 "$nvmfpid" 2> /dev/null; then
			die "nvmf_tgt exited during startup (pid $nvmfpid)"
		fi
		sleep 0.25
	done
	die "target failed to come up on $RPC_SOCK"
}

# Create a thing, tolerating "already exists" so re-runs are idempotent-ish.
rpc_idempotent() {
	local out
	if out=$(rpc "$@" 2>&1); then
		printf '%s\n' "$out"
		return 0
	fi
	if grep -qiE 'already exists|File exists|already in use|duplicate' <<< "$out"; then
		echo "### (already present) $*" >&2
		return 0
	fi
	echo "$out" >&2
	return 1
}

# ---- wire up -------------------------------------------------------------
start_or_connect_target

# Transport (idempotent across re-runs / shared target).
rpc_idempotent nvmf_create_transport -t VFIOUSER > /dev/null

# One named rados cluster handle for the whole tier.
rpc_idempotent kvdev_rados_register_cluster "$CLUSTER_NAME" \
	--user "$CEPH_USER" --config-file "$CEPH_CONF" --key-file "$CEPH_KEYRING" > /dev/null

# One subsystem for the tier. Track whether we created it (for teardown policy).
if rpc nvmf_get_subsystems 2> /dev/null | grep -q "\"$NQN\""; then
	echo "### subsystem $NQN already exists; reusing"
else
	rpc nvmf_create_subsystem "$NQN" -s "$SERIAL" -a
	created_subsystem=1
fi

# Per-tenant: kvdev bound to the tenant rados namespace, then a KV namespace.
declare -a map_nsid map_tenant map_ns
idx=0
for spec in "${tenants[@]}"; do
	tenant="${spec%%:*}"
	radosns="${spec#*:}"
	if [[ -z $tenant || -z $radosns || $tenant == "$spec" ]]; then
		die "bad tenant spec '$spec' (expected TENANT:RADOS_NS)"
	fi
	idx=$((idx + 1))
	kvdev="KvOff_${tenant}"

	rpc_idempotent kvdev_rados_create "$kvdev" "$CLUSTER_NAME" "$KV_POOL" \
		--namespace "$radosns" > /dev/null

	# Request an explicit, deterministic nsid (1..N in tenant order) so the
	# host-facing nsid->tenant map is stable across runs. The add_kv_ns RPC
	# returns the assigned nsid, but the CLI wrapper does not echo it, so we
	# confirm the binding by reading it back from nvmf_get_subsystems below.
	if ! rpc_idempotent nvmf_subsystem_add_kv_ns "$NQN" "$kvdev" -n "$idx" > /dev/null; then
		die "failed to add KV ns for tenant $tenant ($kvdev)"
	fi

	# Read back the nsid actually bound to this kvdev under the subsystem.
	nsid=$(kvdev_nsid "$NQN" "$kvdev")
	[[ -n $nsid && $nsid != "0" ]] || die "KV ns for tenant $tenant ($kvdev) not found after add"

	map_nsid+=("$nsid")
	map_tenant+=("$tenant")
	map_ns+=("$radosns")
done

# Single vfio-user listener for the subsystem (all nsids are reachable through it).
rpc_idempotent nvmf_subsystem_add_listener "$NQN" -t VFIOUSER -a "$muser_dir" -s 0 > /dev/null

# ---- report --------------------------------------------------------------
echo
echo "=== KV offload target ready (ADR-0006 tenancy) ==="
echo "subsystem NQN : $NQN"
echo "rados pool    : $KV_POOL"
echo "RPC socket    : $RPC_SOCK"
echo "vfio-user sock: $muser_dir   (a co-located host attaches to THIS path)"
echo
echo "nsid -> (tenant) -> rados namespace:"
printf '  %-6s %-20s %s\n' "nsid" "tenant" "rados-namespace"
for i in "${!map_nsid[@]}"; do
	printf '  %-6s %-20s %s\n' "${map_nsid[$i]}" "${map_tenant[$i]}" "${map_ns[$i]}"
done
echo "=================================================="

if [[ $KEEP -eq 1 ]]; then
	echo
	echo "### --keep set: target left running (pid ${nvmfpid:-<external>}). Attach with the vfio-user sock above."
	echo "### To tear down later: $RPC_PY_BIN -s $RPC_SOCK nvmf_delete_subsystem $NQN ; kill ${nvmfpid:-<pid>}"
fi
