# Changelog

## 0.1.0 (unreleased)

- The v2 interface begins beside the frozen v1 interface
  ([ADR 0017](docs/decisions/0017-v1-frozen-under-legacy.md)).
- The shader ABI module `Shaders/RojoRHI/RojoRHI.slang`, with compile-only conformance shaders for
  MSL, SPIR-V and HLSL ([ADR 0018](docs/decisions/0018-root-srv-and-reserved-bindings.md)).
- The public headers `<rojoRHI/Addr.h>` and `<rojoRHI/IndirectArgs.h>`.
- A build-time check of shared struct layouts against Slang reflection on every target.
- The Null backend: devices with frame slots and deferred destruction, generation-checked handles,
  capabilities, format capabilities, limits and the message callback, over a backend-neutral core
  ([architecture](docs/architecture/null-backend.md)).
- The Debug-tier `ROJORHI_VU` check, death tests for the valid-usage rules of those entries and
  `Tools/check_vu_coverage.py`, which requires a death test or a pending entry for every rule.
