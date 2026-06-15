# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Intel Corporation. All rights reserved.
#
# Standalone build for the rados-nkvx executor service skeleton (Slice C2) and
# its test client. NOT part of the SPDK build graph: the executor is non-SPDK by
# design (docs/design/slice-c-exec-rpc-mercury.md §1). Both binaries link the
# shared C3 contract (../nkvx_exec_rpc.c) directly against Mercury and include
# the in-tree spdk/kvdev.h (status enum only).
#
# Usage:
#   make -f Makefile [MERCURY_PREFIX=<mercury-install>]      # build
#   make -f Makefile run                                     # build + loopback test
#   make -f Makefile clean
# MERCURY_PREFIX defaults to the worktree's vendored install (scripts/vendor-mercury.sh).

SPDK_ROOT       ?= $(realpath $(CURDIR)/../../../..)
RADOS_DIR       := $(SPDK_ROOT)/module/kvdev/rados
MERCURY_PREFIX  ?= $(SPDK_ROOT)/vendor/mercury-install

MERCURY_CFLAGS  := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --cflags mercury)
MERCURY_LIBS    := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --libs mercury)

# -std=gnu11 is LOAD-BEARING: the C3 wire-status SPDK_STATIC_ASSERTs in
# ../nkvx_exec_rpc.c only fire when <assert.h> defines static_assert as a macro
# (true under gnu11, NOT under the compiler default C23). The executor MUST build
# the contract under gnu11 or the drift guard is decorative (design §3).
CFLAGS  := -g -O0 -std=gnu11 -Wall -Wextra -Werror \
           -I$(SPDK_ROOT)/include -I$(RADOS_DIR) $(MERCURY_CFLAGS)
LDFLAGS := $(MERCURY_LIBS) -Wl,-rpath,$(MERCURY_PREFIX)/lib

CONTRACT := $(RADOS_DIR)/nkvx_exec_rpc.c
HEADER   := $(RADOS_DIR)/nkvx_exec_rpc.h

# Slice C5a: the executor backend cold-fills objects from RADOS (cold path only,
# ADR-0009), so the service binary links librados. The client never touches RADOS.
SERVICE := nkvx_service
CLIENT  := nkvx_exec_client
SERVICE_SRCS := nkvx_service.c nkvx_executor.c $(CONTRACT)
SERVICE_LIBS := -lrados

all: $(SERVICE) $(CLIENT)

$(SERVICE): $(SERVICE_SRCS) nkvx_executor.h $(HEADER)
	$(CC) $(CFLAGS) -o $@ $(SERVICE_SRCS) $(SERVICE_LIBS) $(LDFLAGS)

$(CLIENT): nkvx_exec_client.c $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -o $@ nkvx_exec_client.c $(CONTRACT) $(LDFLAGS)

run: all
	MERCURY_PREFIX=$(MERCURY_PREFIX) ./nkvx_service_test.sh

clean:
	rm -f $(SERVICE) $(CLIENT)

.PHONY: all run clean
