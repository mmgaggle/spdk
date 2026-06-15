#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = kvdev_rados.c kvdev_rados_rpc.c kvdev_rados_nkvx.c kvdev_rados_nkvx_wasm.c
LIBNAME = kvdev_rados

# rados-nkvx wasmtime Exec path (ADR-0013). With --with-wasm the dlopen-backed
# wasm executor in kvdev_rados_nkvx.c is compiled; the vendored wasmtime C-API
# headers supply the signatures. There is NO wasmtime link dependency:
# libwasmtime.so is dlopen()'d at runtime (the only extra need is -ldl, already
# linked by SPDK). With --without-wasm the same source compiles built-ins only,
# guarded by SPDK_CONFIG_WASM in config.h.
ifeq ($(CONFIG_WASM),y)
CFLAGS += -I$(CURDIR)/wasmtime/include
endif

# Slice C inter-tier Exec RPC (ADR-0015, docs/design/slice-c-exec-rpc-mercury.md).
# With --with-mercury the two-tier front (Mercury client + SPDK poller, C4), the
# front bridge, and the shared RPC contract (C3) are compiled in, so nkvx Exec
# can be forwarded to the standalone rados-nkvx executor (C2). Without it, none
# of this builds and the kvdev runs single-tier (in-process), byte-identical to
# a stock build (every Mercury site in the .c files is under SPDK_CONFIG_MERCURY).
# The contract files compile under SPDK's -std=gnu11, which is load-bearing for
# the wire-status static-asserts (see nkvx_exec_rpc.c). Mercury headers resolve
# via pkg-config against the --with-mercury=DIR prefix; the matching link libs
# are added in mk/spdk.modules.mk.
ifeq ($(CONFIG_MERCURY),y)
C_SRCS += nkvx_exec_rpc.c nkvx_front_client.c kvdev_rados_nkvx_front.c
MERCURY_PKG_PATH := $(if $(CONFIG_MERCURY_DIR),$(CONFIG_MERCURY_DIR)/lib/pkgconfig:$(CONFIG_MERCURY_DIR)/lib64/pkgconfig:)$(PKG_CONFIG_PATH)
CFLAGS += $(shell PKG_CONFIG_PATH=$(MERCURY_PKG_PATH) pkg-config --cflags mercury)
endif

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
