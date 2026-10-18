# ADR 0015: Versioning

**Status**: Superseded by [ADR 0022](0022-versioning-in-the-single-header.md)

## Context

RojoRHI is built from source by its consumers, usually as a submodule, so no binary crosses a
compiler boundary and no binary compatibility can be promised or broken. The v2 interface is a
clean break from v1, and it has two backends to bring up before it is stable. Consumers need a
version they can pin that is more meaningful than a bare commit, and shader libraries need a
version that is independent of the library's.

## Decision

- Semantic Versioning 2.0 governs source compatibility. No binary compatibility is promised.
- v2 ships as `0.y.z` until both production backends pass the conformance suite and the consumer
  port is complete; a breaking change bumps the minor version. `1.0.0` follows. Afterwards a break
  needs a major version and a superseding ADR.
- `rojoRHI/Version.h` defines `ROJORHI_VERSION_MAJOR`, `ROJORHI_VERSION_MINOR` and
  `ROJORHI_VERSION_PATCH`.
- Releases are tagged `vX.Y.Z`, so a consumer can pin a tag rather than a bare commit.
- The shader ABI is versioned separately by `ROJORHI_SHADER_ABI_VERSION`, embedded in compiled
  shader libraries and checked at load ([0011](0011-shader-abi.md)).
- `CHANGELOG.md` records breaking changes per minor version.

## Consequences

- `rojoRHI/Version.h` and `CHANGELOG.md` do not exist yet. They arrive with the first v2 code, and
  until a release is tagged, nothing here describes a published release.
- Each chapter of the API reference states the API version it describes, so a reader can match a
  chapter to the tag they pinned.
- A change that only touches the shader ABI bumps `ROJORHI_SHADER_ABI_VERSION` and invalidates
  compiled libraries; it is not by itself a break of the library's source interface.
- Before `1.0.0` the minor version is the compatibility line, so consumers pin a tag and read the
  changelog before moving it.
