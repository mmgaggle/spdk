/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/**
 * \file
 * In-process NVMe Key-Value host (initiator) client shim.
 *
 * A small, reusable, in-process NVMe KV host (initiator) client over the SPDK
 * NVMe driver. It attaches to an NVMf controller over the VFIOUSER transport,
 * binds a Key-Value namespace, and exposes synchronous Store/Retrieve/Exist/
 * Delete primitives backed by SPDK-DMA buffers and a qpair poll loop.
 *
 * This header is the public C ABI that downstream consumers (e.g. the NIXL
 * RADOS_KV plugin, which is C++) link against, so it is a plain C API wrapped
 * in extern "C".
 *
 * Return convention for the op functions (store/retrieve/exist/delete):
 *   -  0 on SUCCESS (NVMe status code 0x00).
 *   -  a POSITIVE NVMe status code (sc) for a device-reported logical status,
 *      e.g. 0x87 (SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST). The status-code type
 *      (sct) is asserted to be GENERIC; a non-generic sct is reported as a
 *      negated errno (-EIO).
 *   -  a NEGATED errno for submit-/transport-level errors (e.g. the negated
 *      return of the underlying spdk_nvme_kv_* submit call, or -EIO).
 */

#ifndef KV_HOST_SHIM_H
#define KV_HOST_SHIM_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque shim handle. */
struct kv_host_shim;

/**
 * Size-versioned open options. Callers MUST set \c opts_size to
 * sizeof(struct kv_host_shim_opts) before calling kv_host_shim_open() so the
 * shim can stay ABI-compatible as fields are added.
 */
struct kv_host_shim_opts {
	/** Size of this struct as known to the caller. Must be set first. */
	size_t		opts_size;
	/** SPDK env name (used only when init_env is true). May be NULL. */
	const char	*name;
	/** VFIOUSER transport address (the directory containing the socket). */
	const char	*vfu_addr;
	/** Namespace id to bind; 0 selects the first CSI==KV namespace. */
	uint32_t	nsid;
	/**
	 * When true, the shim calls spdk_env_init() in open() and
	 * spdk_env_fini() in close(). When false, the caller owns the SPDK env
	 * (must have initialized it already).
	 */
	bool		init_env;
};

/**
 * Open a KV host shim: probe/attach the controller at opts->vfu_addr over
 * VFIOUSER, bind the KV namespace, allocate an I/O qpair, and cache the KV
 * namespace key/value max lengths.
 *
 * \param opts Size-versioned options (opts_size must be set).
 * \param out  Receives the new shim handle on success.
 *
 * \return 0 on success, a negated errno on failure.
 */
int kv_host_shim_open(const struct kv_host_shim_opts *opts, struct kv_host_shim **out);

/** Close a shim opened by kv_host_shim_open(). Safe to call with NULL. */
void kv_host_shim_close(struct kv_host_shim *sh);

/** Allocate a DMA-capable buffer of \c len bytes (zeroed). NULL on failure. */
void *kv_host_shim_dma_alloc(size_t len);

/** Free a buffer returned by kv_host_shim_dma_alloc(). Safe with NULL. */
void kv_host_shim_dma_free(void *buf);

/** Maximum value length (kvvml) advertised by the bound KV namespace. */
uint32_t kv_host_shim_max_value_len(const struct kv_host_shim *sh);

/** Maximum key length (kvkml) advertised by the bound KV namespace. */
uint32_t kv_host_shim_max_key_len(const struct kv_host_shim *sh);

/**
 * KV Store \c value (\c value_len bytes) under \c key. \c value must be a
 * DMA-capable buffer (from kv_host_shim_dma_alloc()).
 *
 * \return per the return convention documented at the top of this header.
 */
int kv_host_shim_store(struct kv_host_shim *sh, const void *key, uint8_t key_len,
		       const void *value, uint32_t value_len);

/**
 * KV Retrieve the value for \c key into \c value (\c buf_len bytes). \c value
 * must be a DMA-capable buffer. On SUCCESS \c *value_len_out (when non-NULL) is
 * set to the device's TRUE value length (the completion cdw0), which may exceed
 * \c buf_len if the buffer was too small (the returned data is then truncated to
 * \c buf_len bytes, matching the NVMe KV Retrieve contract).
 *
 * \return per the return convention documented at the top of this header.
 */
int kv_host_shim_retrieve(struct kv_host_shim *sh, const void *key, uint8_t key_len,
			  void *value, uint32_t buf_len, uint32_t *value_len_out);

/**
 * KV Exist for \c key. Returns 0 if present, 0x87 if absent (per convention).
 */
int kv_host_shim_exist(struct kv_host_shim *sh, const void *key, uint8_t key_len);

/**
 * KV Delete \c key. Returns 0 on success, 0x87 if absent (per convention).
 */
int kv_host_shim_delete(struct kv_host_shim *sh, const void *key, uint8_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* KV_HOST_SHIM_H */
