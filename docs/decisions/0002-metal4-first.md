# ADR 0002: Metal 4 native is the first backend

**Status**: Accepted (2026-09-19)

**Restates**: Luminex ADR 0002, "Metal 4 native as the first RHI backend"

## Context

The only development and validation machine is Apple Silicon macOS. A native backend gets Xcode GPU
debugging of the library's own calls, with no translation layer in between. Metal 4's allocator,
argument-table and residency model is the most modern of the three current explicit APIs, which
keeps the interface honest about what an explicit API costs. metal-cpp is an official Apple
repository with complete Metal 4 coverage, so no Objective-C++ glue layer is needed.

## Decision

Metal 4, through metal-cpp, is RojoRHI's first backend, rather than Vulkan through a translation
layer. `MTLGPUFamilyMetal4` is checked at device creation and is a hard requirement: there is no
Metal 3 fallback, and a device without that family fails creation with an unsupported-device error.
The backend runs 3 frames in flight.

Two details here are read from the implementation as it stands rather than from the source ADR,
which states neither: the frames-in-flight count of 3, and the specific unsupported-device error
category creation returns when the family check fails.

## Consequences

There is no Windows or Linux support, and none until a second backend exists. Interface concepts
are derived from Metal 4 first — argument tables, residency sets, frame-slot retirement — and must
be re-validated for portability when a second backend is implemented. The frames-in-flight count is
a fixed property of the backend rather than a caller choice, so resource lifetime and recycling
rules in the public interface are stated against it.
