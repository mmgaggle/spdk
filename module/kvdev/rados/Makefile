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

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
