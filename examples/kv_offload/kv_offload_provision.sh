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
# The script is idempotent: re-running against an existing RPC socket reuses the
# running target, and per-tenant bindings that already match the intended kvdev
# are skipped. A re-run with the SAME tenant list converges to the same nsid->
# tenant map and exits 0; a conflicting binding (nsid already bound to a
# DIFFERENT kvdev) fails fast with a clear message. A trap tears down anything
# THIS invocation started.

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
  --rados-bin PATH      rados CLI used by --selftest          (env RADOS_BIN,
                        default: rados)

Self-test (tenant-isolation check):
  --selftest            after wiring up, Store a value to nsid 1 via the
                        in-process KV host shim (test/nvmf/kv_shim) and confirm
                        with the rados CLI that the resulting object lands ONLY
                        in tenant 1's rados namespace (present there, absent
                        from tenant 2's namespace when >=2 tenants). Requires
                        the kv_shim_test binary (built under test/nvmf/kv_shim)
                        and rados CLI access to the cluster. Implies the target
                        is left up only as long as the script runs.

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
SELFTEST=0
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
		--selftest)
			SELFTEST=1
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

# Validate every tenant spec UP FRONT, and reject duplicate tenant names or
# duplicate rados namespaces before touching the target. A duplicate tenant
# would otherwise collide on the kvdev name (KvOff_<tenant>) and a duplicate
# rados namespace would silently break per-tenant isolation -- both would
# mislabel the nsid->tenant->namespace map. Fail fast with a clear message.
declare -A _seen_tenant _seen_ns
for spec in "${tenants[@]}"; do
	tenant="${spec%%:*}"
	radosns="${spec#*:}"
	if [[ -z $tenant || -z $radosns || $tenant == "$spec" ]]; then
		die "bad tenant spec '$spec' (expected TENANT:RADOS_NS)"
	fi
	if [[ -n ${_seen_tenant[$tenant]:-} ]]; then
		die "duplicate tenant name '$tenant' (each tenant must be unique; specs: ${tenants[*]})"
	fi
	if [[ -n ${_seen_ns[$radosns]:-} ]]; then
		die "duplicate rados namespace '$radosns' (tenant '$tenant' collides with tenant '${_seen_ns[$radosns]}'; each tenant needs its own namespace for isolation)"
	fi
	_seen_tenant[$tenant]=1
	_seen_ns[$radosns]=$tenant
done
unset _seen_tenant _seen_ns

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
# the script works without jq installed. Asserts EXACTLY ONE matching binding
# exists and prints that single nsid; prints nothing (empty) if none match, and
# fails (rc 3) if more than one nsid is bound to the same kvdev -- never returns
# a stale/duplicate nsid via head -n1.
kvdev_nsid() {
	local nqn="$1" kvdev="$2" matches
	if command -v jq > /dev/null 2>&1; then
		matches=$(rpc nvmf_get_subsystems 2> /dev/null | jq -r \
			--arg nqn "$nqn" --arg kv "$kvdev" \
			'.[] | select(.nqn==$nqn) | .namespaces[]?
			 | select(.kvdev_name==$kv) | .nsid')
	else
		matches=$(rpc nvmf_get_subsystems 2> /dev/null | python3 -c '
import json,sys
nqn,kv=sys.argv[1],sys.argv[2]
for s in json.load(sys.stdin):
    if s.get("nqn")==nqn:
        for ns in s.get("namespaces",[]):
            if ns.get("kvdev_name")==kv:
                print(ns.get("nsid"))
' "$nqn" "$kvdev")
	fi
	local n
	n=$(grep -c . <<< "$matches" 2> /dev/null || echo 0)
	[[ -z $matches ]] && n=0
	if [[ $n -gt 1 ]]; then
		echo "$prog: kvdev '$kvdev' is bound to multiple nsids ($(tr '\n' ' ' <<< "$matches")) under $nqn" >&2
		return 3
	fi
	printf '%s\n' "$matches"
}

# Return the kvdev_name bound to a given nsid under a subsystem (empty if the
# nsid is unbound). Used to decide idempotency before adding a KV namespace.
nsid_kvdev() {
	local nqn="$1" want="$2"
	if command -v jq > /dev/null 2>&1; then
		rpc nvmf_get_subsystems 2> /dev/null | jq -r \
			--arg nqn "$nqn" --argjson n "$want" \
			'.[] | select(.nqn==$nqn) | .namespaces[]?
			 | select(.nsid==$n) | .kvdev_name // ""'
	else
		rpc nvmf_get_subsystems 2> /dev/null | python3 -c '
import json,sys
nqn=sys.argv[1]; want=int(sys.argv[2])
for s in json.load(sys.stdin):
    if s.get("nqn")==nqn:
        for ns in s.get("namespaces",[]):
            if ns.get("nsid")==want:
                print(ns.get("kvdev_name") or "")
' "$nqn" "$want"
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

	# Genuine idempotency: inspect what (if anything) nsid $idx is already bound
	# to under this subsystem BEFORE trying to add it. nvmf_subsystem_add_kv_ns
	# on an already-bound nsid returns JSON-RPC -32602 "Invalid parameters",
	# which is NOT an "already exists" string -- so we must not blindly call it.
	bound_kvdev=$(nsid_kvdev "$NQN" "$idx")
	if [[ -n $bound_kvdev ]]; then
		if [[ $bound_kvdev == "$kvdev" ]]; then
			# nsid $idx already maps to THIS tenant's kvdev: nothing to do.
			# The kvdev itself necessarily already exists (it is bound), so we
			# do NOT recreate it -- this is the idempotent re-run path.
			echo "### nsid $idx already bound to $kvdev (tenant $tenant); skipping (idempotent)" >&2
			nsid="$idx"
			map_nsid+=("$nsid")
			map_tenant+=("$tenant")
			map_ns+=("$radosns")
			continue
		fi
		die "nsid $idx is already bound to a DIFFERENT kvdev '$bound_kvdev' (intended '$kvdev' for tenant '$tenant'); refusing to clobber an existing mapping -- tear down the existing subsystem or use a different --nqn"
	fi

	# nsid $idx is unbound. Create the per-tenant kvdev. Because tenant names are
	# validated unique up front, a "already exists" kvdev here while its nsid is
	# unbound indicates a stale/orphaned device from a prior run with a different
	# nsid layout (or a real bug) -- treat it as fatal rather than swallowing it,
	# so we never silently bind two nsids to one namespace.
	if ! out=$(rpc kvdev_rados_create "$kvdev" "$CLUSTER_NAME" "$KV_POOL" \
		--namespace "$radosns" 2>&1); then
		die "kvdev_rados_create for tenant '$tenant' ($kvdev) failed: $out"
	fi

	# Request an explicit, deterministic nsid (1..N in tenant order) so the
	# host-facing nsid->tenant map is stable across runs. The add_kv_ns RPC
	# returns the assigned nsid, but the CLI wrapper does not echo it, so we
	# confirm the binding by reading it back from nvmf_get_subsystems below.
	if ! out=$(rpc nvmf_subsystem_add_kv_ns "$NQN" "$kvdev" -n "$idx" 2>&1); then
		die "nvmf_subsystem_add_kv_ns for tenant '$tenant' ($kvdev, nsid $idx) failed: $out"
	fi

	# Read back the nsid actually bound to this kvdev under the subsystem.
	# kvdev_nsid asserts exactly one binding (no stale/duplicate via head -n1).
	nsid=$(kvdev_nsid "$NQN" "$kvdev") || die "ambiguous nsid for tenant $tenant ($kvdev)"
	[[ -n $nsid && $nsid != "0" ]] || die "KV ns for tenant $tenant ($kvdev) not found after add"
	[[ $nsid == "$idx" ]] || die "tenant $tenant ($kvdev) bound to nsid $nsid, expected $idx"

	map_nsid+=("$nsid")
	map_tenant+=("$tenant")
	map_ns+=("$radosns")
done

# Single vfio-user listener for the subsystem (all nsids are reachable through it).
# Adding a listener that already exists returns JSON-RPC -32602 (not an "already
# exists" string), so on a re-run we must check first to stay idempotent.
listener_exists() {
	if command -v jq > /dev/null 2>&1; then
		rpc nvmf_subsystem_get_listeners "$NQN" 2> /dev/null | jq -e \
			--arg a "$muser_dir" \
			'.[] | select(.address.trtype=="VFIOUSER" and .address.traddr==$a)' \
			> /dev/null 2>&1
	else
		rpc nvmf_subsystem_get_listeners "$NQN" 2> /dev/null | python3 -c '
import json,sys
a=sys.argv[1]
ls=json.load(sys.stdin)
sys.exit(0 if any(l.get("address",{}).get("trtype")=="VFIOUSER" and
                  l.get("address",{}).get("traddr")==a for l in ls) else 1)
' "$muser_dir"
	fi
}
if listener_exists; then
	echo "### vfio-user listener on $muser_dir already present; skipping (idempotent)" >&2
else
	rpc nvmf_subsystem_add_listener "$NQN" -t VFIOUSER -a "$muser_dir" -s 0 > /dev/null \
		|| die "failed to add vfio-user listener on $muser_dir"
fi

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

# ---- optional self-test (tenant isolation) -------------------------------
# Store a value to nsid 1 via the in-process KV host shim, then use the rados
# CLI to confirm the resulting object lands ONLY in tenant 1's rados namespace
# (present there; absent from tenant 2's namespace when there is one). This
# proves the nsid->namespace mapping actually isolates tenants in rados.
run_selftest() {
	local shim_dir="$rootdir/test/nvmf/kv_shim"
	local shim_bin="$shim_dir/kv_shim_test"

	echo
	echo "### --selftest: proving nsid 1 isolates to tenant '${map_tenant[0]}' (ns '${map_ns[0]}')"

	if [[ ! -x $shim_bin ]]; then
		echo "### building kv host shim test ($shim_dir)" >&2
		make -C "$shim_dir" > /dev/null || die "selftest: failed to build $shim_bin"
	fi
	command -v "$RADOS_BIN" > /dev/null 2>&1 || die "selftest: rados CLI not found: $RADOS_BIN (set --rados-bin/RADOS_BIN)"

	# kv_shim_test attaches nsid 0 (== first KV namespace == nsid 1) and Stores
	# keys "kvshim-big" (left present) and "kvshim-tiny" (deleted). We assert on
	# "kvshim-big": its rados oid is the lowercase-hex encoding of the key bytes
	# (kvdev_rados_key_to_oid). Run the shim and require its PASS line.
	local log="$DOMAIN_DIR/selftest.kv_shim_test.log"
	if ! "$shim_bin" "$muser_dir" > "$log" 2>&1 || ! grep -q "kv_shim_test: PASS" "$log"; then
		echo "--- kv_shim_test output ---" >&2
		cat "$log" >&2 || true
		die "selftest: kv_shim_test did not PASS against $muser_dir"
	fi
	echo "### kv_shim_test PASS (Store/Retrieve to nsid 1)"

	local oid
	oid=$(printf '%s' "kvshim-big" | od -An -tx1 | tr -d ' \n')
	local rcli=("$RADOS_BIN" -c "$CEPH_CONF" -k "$CEPH_KEYRING" -p "$KV_POOL")

	# Present in tenant 1's namespace.
	if ! "${rcli[@]}" -N "${map_ns[0]}" stat "$oid" > /dev/null 2>&1; then
		die "selftest: object '$oid' NOT found in tenant 1 namespace '${map_ns[0]}' (Store did not land where expected)"
	fi
	echo "### object present in rados namespace '${map_ns[0]}' (nsid 1) -- correct"

	# Absent from every OTHER tenant's namespace (proves isolation).
	local i
	for i in "${!map_ns[@]}"; do
		[[ $i -eq 0 ]] && continue
		if "${rcli[@]}" -N "${map_ns[$i]}" stat "$oid" > /dev/null 2>&1; then
			die "selftest: ISOLATION FAILURE -- object '$oid' also present in tenant $((i + 1)) namespace '${map_ns[$i]}'"
		fi
		echo "### object absent from rados namespace '${map_ns[$i]}' (nsid $((i + 1))) -- isolated"
	done

	# Clean up the object we created so the pool is left pristine.
	"${rcli[@]}" -N "${map_ns[0]}" rm "$oid" > /dev/null 2>&1 || true
	echo "### selftest PASS: nsid 1 isolates to namespace '${map_ns[0]}' (test object removed)"
}

if [[ $SELFTEST -eq 1 ]]; then
	run_selftest
fi

if [[ $KEEP -eq 1 ]]; then
	echo
	echo "### --keep set: target left running (pid ${nvmfpid:-<external>}). Attach with the vfio-user sock above."
	echo "### To tear down later:"
	echo "###   $RPC_PY_BIN -s $RPC_SOCK nvmf_delete_subsystem $NQN ; kill ${nvmfpid:-<pid>}"
	if [[ $owns_domain_dir -eq 1 ]]; then
		echo "###   rm -rf $DOMAIN_DIR   # remove the minted domain dir this run owns"
	fi
fi
