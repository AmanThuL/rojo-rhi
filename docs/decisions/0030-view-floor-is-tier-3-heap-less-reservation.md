# ADR 0030: the `maxViews` floor is tier 3's heap less the D3D12 reservation

**Status**: Accepted (2026-11-12)

## Context

The API reference set the portable `maxViews` floor at 1,000,000, the figure resource binding tier
3 promises on D3D12 and the research figure for Metal argument buffers. On D3D12 the backend keeps
leading slots of the one shader-visible view heap for itself: slots 0 and 1, which hold the null
raw views the zero `Addr` reaches, and the transient ring of `framesInFlight` × 1,024 slots, 2,050
or 3,074 in all. They never reach a caller, so `maxViews` is the heap less them, and a `maxViews`
of 1,000,000 needed a heap of 1,002,050 or more.

Tier 3 promises 1,000,000 descriptors in the heap, not 1,000,000 beside whatever a library
reserves. WARP 1.0.21 reports a `D3D12_FEATURE_D3D12_OPTIONS19` maximum of 2,097,152, so the
overcount did not show on hosted CI. The first hardware run did show it: an NVIDIA GeForce RTX
3080 Ti on driver 621.14 reports a maximum of exactly 1,000,000, and `createDevice` refused it
with `Unsupported` naming `maxViews`, so no case could run
(`rojo-rhi-evidence/d3d12-t1/2026-11-12/`). A floor that refuses tier 3 hardware contradicts
[ADR 0010](0010-hardware-floor.md), which admits it.

## Decision

- The portable `maxViews` floor is 996,926: tier 3's heap of 1,000,000 less the largest D3D12
  reservation, 2 + 3 × 1,024 at a `framesInFlight` of 3. A device reports its own value, at least
  the floor.
- The D3D12 backend refuses a device whose `OPTIONS19` maximum is below 1,000,000, the tier's heap,
  and creates a heap of 1,000,000 on a device that does not report the maximum. `maxViews` stays
  the heap less the reservation.
- Metal 4 and the Null backend keep reporting 1,000,000, which is above the floor.
- ADR 0010 is not edited; it names tier 3 and no view count. The reference's floor rows and the
  contract cases state the new figure.

## Consequences

- A tier 3 device whose heap is exactly 1,000,000 is admitted, with `maxViews` 997,950 at a
  `framesInFlight` of 2 and 996,926 at 3.
- A caller that relied on 1,000,000 portable view slots has 3,074 fewer at worst. No consumer
  reserves near the floor; Luminex's tests that hold every slot read `maxViews` from the device.
- Raising the reservation later, a larger ring or more held slots, lowers the floor again and
  needs a decision record of its own.
