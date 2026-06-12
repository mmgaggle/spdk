---
status: accepted
---

# The Exec ABI names a (runtime, artifact), not a WASM module

The Exec binding references an executable as `(runtime, artifact-hash, caps)` rather
than assuming WebAssembly. `runtime` selects the execution backend — wasmtime in v1,
with room for alternates (notably a **Velox plan** for vectorized columnar work) — and
`artifact-hash` is the content hash of that runtime's artifact (a `.wasm` binary, a
serialized query plan, …). WASM is the only runtime built in v1; the generalization
costs nothing today but turns "add a runtime later" into a new artifact *kind* rather
than a re-freeze of the tenant-facing ABI.

## Why

We expect more than one execution model. WASM is the general, sandboxed path: untrusted
*code* made safe by the sandbox, suited to arbitrary algorithms (graph traversal,
custom kernels). Velox is the opposite trade: trusted *operators* + an untrusted *plan*
(data over an allowlisted operator set, no arbitrary code), native-vectorized over the
Arrow on-object format — far faster for columnar gather/filter/projection/aggregation
and many KV-cache transforms, and it sidesteps the WASM/memory64 penalty. The two are
complementary, not competing, so the ABI must host both.

## Consequences

- **Do not nest.** Velox-in-WASM is rejected: folly + its dependency stack won't target
  a no-imports sandbox, AVX degrades to WASM SIMD128, parallelism is lost — paying
  WASM's penalty while discarding Velox's only advantage. Runtimes are siblings behind
  the ABI, dispatched by `runtime` type.
- The control-plane `binding` and the executor's artifact cache key on `(runtime, hash)`.
- Pursuing Velox depends on unknowns to research first: an embeddable single-threaded
  mode, zero-copy Arrow→Velox import, plan serialization (Substrait or native) as the
  artifact, and footprint next to an SPDK process.
