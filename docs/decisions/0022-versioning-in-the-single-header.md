# ADR 0022: Versioning, with the version in the single public header

**Status**: Accepted (2026-10-18)

**Supersedes**: [ADR 0015](0015-versioning.md), whose decision it restates with one change: the
version macros live in `<rojoRHI/RHI.h>`, not in a header of their own.

## Context

[ADR 0015](0015-versioning.md) named `rojoRHI/Version.h` as the header that defines the library
version. The v2 public interface has since become one header, `Include/rojoRHI/RHI.h`, in sections
by API-reference chapter, so it reads alongside the reference and browses more easily than one
small file per type. A separate version header would be the only exception. An accepted ADR is
immutable, so the change is a new ADR.

The reasons behind ADR 0015 still hold. RojoRHI is built from source by its consumers, usually as
a submodule, so no binary crosses a compiler boundary and no binary compatibility can be promised
or broken. Consumers need a version they can pin that is more meaningful than a bare commit, and
shader libraries need a version that is independent of the library's.

## Decision

- Semantic Versioning 2.0 governs source compatibility. No binary compatibility is promised.
- v2 ships as `0.y.z` until both production backends pass the conformance suite and the consumer
  port is complete; a breaking change bumps the minor version. `1.0.0` follows. Afterwards a break
  needs a major version and a superseding ADR.
- `<rojoRHI/RHI.h>` defines `ROJORHI_VERSION_MAJOR`, `ROJORHI_VERSION_MINOR` and
  `ROJORHI_VERSION_PATCH`, in its first section.
- Releases are tagged `vX.Y.Z`, so a consumer can pin a tag rather than a bare commit.
- The shader ABI is versioned separately by `ROJORHI_SHADER_ABI_VERSION`, embedded in compiled
  shader libraries and checked at load ([0011](0011-shader-abi.md)).
- `CHANGELOG.md` records breaking changes per minor version.

## Consequences

- A consumer that reads the version includes the whole public interface; it has no lighter header
  to include for the macros alone.
- Until a release is tagged, nothing here describes a published release.
- Each chapter of the API reference states the API version it describes, so a reader can match a
  chapter to the tag they pinned.
- A change that only touches the shader ABI bumps `ROJORHI_SHADER_ABI_VERSION` and invalidates
  compiled libraries; it is not by itself a break of the library's source interface.
- Before `1.0.0` the minor version is the compatibility line, so consumers pin a tag and read the
  changelog before moving it.
