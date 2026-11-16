# Contributing

Bug reports are welcome. Pull requests are not accepted: this repository carries release snapshots
only, and development happens in a private repository, so a change made here cannot be merged into
the next release.

## Reporting a bug

Open an issue with the bug report form. It asks for:

- **The version**: the release tag you build, such as `v1.0.0`.
- **The backend and build mode**: Metal 4, D3D12 or Null, and Debug or Release.
- **The GPU and its driver**: the GPU model, and the driver version where the platform has one.
- **The operating system** and its version.
- **A minimal reproduction**: the smallest program, or the smallest change to one of the programs
  under `Examples/`, that shows the problem, with what you expected and what happened.
- **The messages**: the output of the validation tracker in Debug with `DeviceDesc::validation` set,
  and of the platform's debug layer (`MTL_DEBUG_LAYER=1` on Metal 4, the debug layer on D3D12).

A report against the API reference (`docs/spec/`), where the reference and the library disagree
or the reference is unclear, is a bug too.

For a security vulnerability, do not open an issue; follow [`SECURITY.md`](SECURITY.md).

## How a fix reaches a release

1. An issue opened here is triaged into the private development tracker, and a comment on it says
   it is tracked for a future release.
2. The fix is made, tested and reviewed there.
3. It ships in the next release: a patch version for a fix that changes no interface, a minor
   version for an addition. The release's entry in [`CHANGELOG.md`](CHANGELOG.md) lists it.
4. The issue here is closed with the version that carries the fix.

## Feature requests

Open an issue describing what you need and why. RojoRHI adds a feature when a consumer has
demonstrably needed it, so the use case matters more than the proposed interface.
