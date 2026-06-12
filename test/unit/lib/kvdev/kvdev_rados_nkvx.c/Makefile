#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = kvdev_rados_nkvx_ut.c

# The UT pulls in the executor AND the dlopen-backed wasm runtime as source. When
# --with-wasm is set the wasm runtime includes the vendored wasmtime C-API headers
# and dlopen's libwasmtime.so at runtime (-ldl). With --without-wasm the wasm
# source compiles its NOT_SUPPORTED stub and neither the headers nor -ldl matter.
ifeq ($(CONFIG_WASM),y)
CFLAGS += -I$(SPDK_ROOT_DIR)/module/kvdev/rados/wasmtime/include
CFLAGS += -DNKVX_UT_WASM_DIR=\"$(SPDK_ROOT_DIR)/test/nvmf/kv/wasm\"
LDFLAGS += -ldl
endif

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
