#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# End-to-end test of the NIXL RADOS_KV plugin against SPDK + a vstart Ceph
# cluster. Runs the full "5-step" procedure as one command:
#
#   1. Ensure a vstart Ceph cluster is up and the KV pool/namespace exist.
#   2. Verify the SPDK target (nvmf_tgt) + KV host shim are built.
#   3. Build the NIXL RADOS_KV plugin (in a dedicated detached worktree so the
#      user's nixl checkout is never disturbed).
#   4. Run the librados round-trip: Store/Retrieve/Exist over a vfio-user KV
#      namespace, with rados-CLI verification that the value landed in RADOS.
#   5. Provision two per-tenant KV namespaces and prove cross-tenant isolation.
#
# Everything is parameterized via env vars (defaults match the dev box). The
# script is idempotent: a live cluster / prior build / existing resources are
# reused, not clobbered.
#
# Usage:
#   bash run_e2e_rados_kv.sh                 # all 5 steps
#   REBUILD=1 bash run_e2e_rados_kv.sh       # force a clean plugin rebuild
#   SKIP_MULTITENANT=1 bash run_e2e_rados_kv.sh   # steps 1-4 only
#
# Key env overrides: SPDK_ROOT, NIXL_ROOT, NIXL_BRANCH, CEPH_BUILD, CEPH_CONF,
#   CEPH_KEYRING, RADOS_BIN, CEPH_USER, KV_POOL, KV_NS, BUILD_WT.

set -euo pipefail

# ---- Configuration (override via environment) -----------------------------
SPDK_ROOT="${SPDK_ROOT:-/mnt/spdk}"
NIXL_ROOT="${NIXL_ROOT:-/mnt/llm-d-work/nixl}"
NIXL_BRANCH="${NIXL_BRANCH:-nvme-kv-rados}"
CEPH_BUILD="${CEPH_BUILD:-/mnt/ceph/build}"
CEPH_CONF="${CEPH_CONF:-$CEPH_BUILD/ceph.conf}"
CEPH_KEYRING="${CEPH_KEYRING:-$CEPH_BUILD/keyring}"
RADOS_BIN="${RADOS_BIN:-$CEPH_BUILD/bin/rados}"
CEPH_USER="${CEPH_USER:-admin}"
KV_POOL="${KV_POOL:-kvpool}"
KV_NS="${KV_NS:-kvns}"
# Dedicated build worktree (detached) so we never touch the user's nixl checkout.
BUILD_WT="${BUILD_WT:-$NIXL_ROOT/.e2e-rados-kv-wt}"
REBUILD="${REBUILD:-0}"
SKIP_MULTITENANT="${SKIP_MULTITENANT:-0}"

log() { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
ok() { printf '\033[1;32m   %s\033[0m\n' "$*"; }
die() {
	printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2
	exit 1
}

log "Config"
printf '   SPDK_ROOT=%s\n   NIXL_ROOT=%s (branch %s)\n   CEPH_BUILD=%s\n   pool/ns=%s/%s  user=%s\n' \
	"$SPDK_ROOT" "$NIXL_ROOT" "$NIXL_BRANCH" "$CEPH_BUILD" "$KV_POOL" "$KV_NS" "$CEPH_USER"

# ---- Step 1: Ceph (vstart) ------------------------------------------------
log "Step 1/5: Ceph cluster (vstart) + pool/namespace"
[ -x "$RADOS_BIN" ] || die "rados CLI not found at $RADOS_BIN (set CEPH_BUILD / RADOS_BIN)"
if timeout 15 "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" -N "$KV_NS" ls > /dev/null 2>&1; then
	ok "Ceph up; pool '$KV_POOL' (ns '$KV_NS') reachable."
else
	echo "   Ceph/pool not reachable; attempting vstart bring-up..."
	if [ -f "$CEPH_BUILD/../src/vstart.sh" ]; then
		(cd "$CEPH_BUILD" && MON=1 OSD=1 MGR=1 ../src/vstart.sh --new -x --localhost --bluestore) \
			|| die "vstart.sh failed"
	else
		die "vstart.sh not found under $CEPH_BUILD/../src; bring Ceph up manually."
	fi
	"$CEPH_BUILD/bin/ceph" -c "$CEPH_CONF" osd pool create "$KV_POOL" 64 > /dev/null 2>&1 || true
	timeout 30 "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" ls > /dev/null 2>&1 \
		|| die "pool '$KV_POOL' still not reachable after bring-up"
	ok "Ceph brought up; pool '$KV_POOL' ready."
fi

# ---- Step 2: SPDK target + KV host shim -----------------------------------
log "Step 2/5: SPDK target (nvmf_tgt) + KV host shim"
[ -x "$SPDK_ROOT/build/bin/nvmf_tgt" ] \
	|| die "nvmf_tgt not built. Run: (cd $SPDK_ROOT && ./configure --with-rbd --with-vfio-user --without-nvme-cuse && make -j\$(nproc))"
[ -f "$SPDK_ROOT/test/nvmf/kv_shim/kv_host_shim.h" ] \
	|| die "kv_host_shim missing under $SPDK_ROOT/test/nvmf/kv_shim (check out the nvme-kv branch)"
ok "nvmf_tgt and kv_host_shim present."

# ---- Step 3: Build the NIXL RADOS_KV plugin -------------------------------
log "Step 3/5: Build NIXL RADOS_KV plugin (branch $NIXL_BRANCH, detached worktree)"
command -v meson > /dev/null || die "meson not installed"
command -v ninja > /dev/null || die "ninja not installed"
git -C "$NIXL_ROOT" rev-parse --verify "$NIXL_BRANCH" > /dev/null 2>&1 \
	|| die "branch '$NIXL_BRANCH' not found in $NIXL_ROOT"

if [ ! -d "$BUILD_WT" ]; then
	git -C "$NIXL_ROOT" worktree add -f --detach "$BUILD_WT" "$NIXL_BRANCH" > /dev/null
	ok "created build worktree at $BUILD_WT"
else
	git -C "$BUILD_WT" reset --hard "$NIXL_BRANCH" > /dev/null 2>&1 \
		|| die "could not sync build worktree $BUILD_WT to $NIXL_BRANCH"
	ok "reused build worktree at $BUILD_WT (synced to $NIXL_BRANCH)"
fi

cd "$BUILD_WT"
TEST_BIN="builddir/src/plugins/rados_kv/rados_kv_roundtrip_test"
if [ "$REBUILD" = "1" ] || [ ! -x "$TEST_BIN" ]; then
	RECONF=""
	[ -d builddir ] && RECONF="--reconfigure"
	meson setup builddir $RECONF \
		-Denable_plugins=RADOS_KV -Drados_kv_build_test=true \
		-Dbuild_tests=false -Dbuild_examples=false -Dbuild_docs=false \
		-Drust=false -Dbuild_python_bindings=false \
		-Dspdk_root="$SPDK_ROOT" -Dspdk_kv_shim_dir="$SPDK_ROOT/test/nvmf/kv_shim"
	ninja -C builddir
else
	ok "plugin already built (set REBUILD=1 to force)"
fi
[ -x "$TEST_BIN" ] || die "plugin round-trip test binary missing after build"
plugin_so=builddir/src/plugins/rados_kv/libplugin_RADOS_KV.so
ok "built $plugin_so ($(stat -c %s "$plugin_so") bytes)"

# ---- Step 4: librados round-trip ------------------------------------------
log "Step 4/5: RADOS_KV round-trip over librados (Store/Retrieve/Exist + rados verify)"
CEPH_CONF="$CEPH_CONF" CEPH_KEYRING="$CEPH_KEYRING" RADOS_BIN="$RADOS_BIN" \
	CEPH_USER="$CEPH_USER" KV_POOL="$KV_POOL" KV_NS="$KV_NS" \
	bash "$BUILD_WT/src/plugins/rados_kv/run_roundtrip_rados.sh" \
	|| die "librados round-trip failed (step 4)"
ok "round-trip PASS (value verified in $KV_POOL/$KV_NS)"

# ---- Step 5: multi-tenant provisioning + isolation selftest ---------------
if [ "$SKIP_MULTITENANT" = "1" ]; then
	log "Step 5/5: multi-tenant selftest SKIPPED (SKIP_MULTITENANT=1)"
else
	log "Step 5/5: multi-tenant provisioning + cross-tenant isolation selftest"
	bash "$SPDK_ROOT/examples/kv_offload/kv_offload_provision.sh" \
		--ceph-conf "$CEPH_CONF" --keyring "$CEPH_KEYRING" --ceph-user "$CEPH_USER" \
		--pool "$KV_POOL" --rados-bin "$RADOS_BIN" \
		--no-huge --mem-size 1024 --selftest \
		"modelA:${KV_NS}" "modelB:${KV_NS}_t2" \
		|| die "multi-tenant provisioning/selftest failed (step 5)"
	ok "two KV namespaces provisioned; cross-tenant isolation proven"
fi

log "ALL STEPS PASSED — NIXL RADOS_KV plugin works e2e against SPDK + vstart Ceph"
