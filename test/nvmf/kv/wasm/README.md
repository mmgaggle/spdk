# rados-nkvx wasm Exec modules (TB-WIMP / ADR-0013)

Precompiled `.wasm` modules for the real-wasm KV Exec path. A KV Exec whose
binding is `nkvx:wasm:<name>` runs `<name>.wasm` from this directory in the
dlopen-backed wasmtime runtime, OFF the SPDK reactor, against a plain copy of the
cold-filled object bytes in wasm linear memory.

## Module ABI

A module exports a linear memory named `memory` and a function named `<name>`:

```
<name>(i32 obj_off, i32 obj_len) -> i32 result_len
```

The host copies the object bytes into linear memory at `obj_off` (256), the
module writes its result at offset 8 and returns the result length, and the host
reads `result_len` bytes back from offset 8. These offsets are fixed in
`module/kvdev/rados/kvdev_rados_nkvx_wasm.c` (`WASM_OBJ_OFF`, `WASM_RES_OFF`).

## bytecount.wasm

Returns the object length as a little-endian `u64` (8 bytes), mirroring the
`bytecount` C built-in. Source: `bytecount.c`.

Rebuild (requires clang with the wasm32 target):

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=bytecount \
  -Wl,--initial-memory=131072 -Wl,--export-memory \
  -o bytecount.wasm bytecount.c
```

## libwasmtime.so (runtime dependency)

wasmtime is loaded at runtime via `dlopen` (NOT linked into SPDK). The executor
probes, in order: `libwasmtime.so` (via ldconfig / `LD_LIBRARY_PATH`),
`/usr/local/lib/libwasmtime.so`, `/usr/lib/libwasmtime.so`,
`/usr/lib64/libwasmtime.so`.

Install the v36 LTS (>= v32) C-API shared library so the target can find it:

```sh
# from wasmtime-vXX-x86_64-linux-c-api.tar.xz (lib/libwasmtime.so):
sudo cp lib/libwasmtime.so /usr/local/lib/libwasmtime.so
```

If `libwasmtime.so` is absent the wasm Exec path fails soft with a distinct
"runtime unavailable" status (NOT_SUPPORTED) — never a crash — and the C built-in
modules continue to work. A vendored copy also lives (gitignored) at
`module/kvdev/rados/wasmtime/lib/libwasmtime.so` after the artifact is unpacked.

The `kv_nkvx_exec.sh` e2e exports `SPDK_NKVX_WASM_DIR` pointing at this directory
so the target can locate `bytecount.wasm`.
