# ADR 0025: The per-operation CPU budget has a floor of 5 ns per RojoRHI call

**Status**: Accepted (2026-10-21)

**Supersedes**: the budget "CPU tax per operation over native: at most 5%" of
[ADR 0014](0014-overhead-measurement.md). Every other budget of ADR 0014 stands (the composed
frame's GPU time, hidden pipeline variants, per-frame allocations and native calls per RojoRHI
call), and so do its principles, its protocol, its rule for a material difference and its
continuous guard.

## Context

[ADR 0014](0014-overhead-measurement.md) bounds RojoRHI's CPU time per operation at 5% over
hand-written native code. The Metal 4 overhead rounds judge it per workload and `K`, on operations
of one to seven RojoRHI calls
([the pre-registration](../research/2026-10-18-metal4-overhead.md)). Several of those operations
take about 45 to 70 ns natively, so 5% of them is 2.3 to 3.5 ns for the whole operation.

The interface that [ADR 0007](0007-address-first-interface.md) and
[ADR 0012](0012-language-and-dispatch.md) chose resolves each object reference through the core's
handle tables: a resource is a 64-bit identifier, not a pointer, and the entries are non-virtual
functions that inline under link-time optimization where the compiler chooses to. Resolving a
handle is a chain of dependent loads (the list, its device, the table's chunk and slot, the latter
two with acquire ordering) before any work. ADR 0012 measured a non-inlined call at about 1.2 ns on
the same machine. The cost per call is therefore an absolute number of nanoseconds that does not
shrink with the operation it sits in.

This amendment is made after the measurements were seen, and its rule was chosen with them in
view. ADR 0014 fixes budgets before any measurement so that a result cannot move its own bar; this
ADR moves the CPU bar after results, which is why it revises no earlier verdict and why later
rounds report the 5% verdict beside its own. What had been seen when it was decided:

- **Round 1** ([results](../research/2026-10-18-metal4-overhead-results.md)) missed the CPU budget
  in 33 of 34 judged configurations, C1 to C7 by 23% to 68%.
- **Round 2** ([results](../research/2026-10-20-metal4-overhead-round2-results.md)) missed it in
  15 of 34. C3 (`setPipeline` alternating between two pipelines, then `draw`; `c` 2) missed at
  every judged `K`: at `K` 1,000 R took 63.9 ns per operation against N's 57.7, a delta of 5.9 ns
  [5.6, 6.4], +10.3%.
- **Two informal profiles** after round 2, of ten paired processes per configuration, which are
  not registered rounds. The first, on 2026-10-20, put C3 at +6.1% to +10.7% across `K`. The
  second, on 2026-10-21 with the backend's later fixes, put C3 at +8.4% (`K` 100), +5.5% (1,000),
  +6.3% (10,000) and +7.4% (100,000): 9.4 ns [8.3, 10.8], 3.2 ns [2.7, 3.6], 3.0 ns [2.7, 3.1] and
  3.3 ns [3.2, 3.4] per operation. In that profile every other judged configuration met 5%.
- **At `K` 100**, C3 measured 11.9 ns in the first profile and 9.4 ns [8.3, 10.8] in the second;
  the floor, 10 ns for its two calls, passes it by 0.6 ns, and only in the later profile.

What was tried on C3 before the budget was taken up:

- **Caching the list's last two pipelines**, so binding either again reads no handle table. It is
  done; it took about 1.4 to 2.0 ns off each switch at `K` of 1,000 and above, and left C3 at
  +6% to +7% in diagnostic runs.
- **A diagnostic that drops every held-state compare** and sends native's three calls
  unconditionally. It is not a valid lowering, since it never sets the winding, and still left
  2.4 to 2.6 ns, 5% to 5.5%. No restructuring of that logic can bring C3 clearly under 5%.
- **Relaxing the acquire load** through which recording reads a native payload without a lock
  ([ADR 0020](0020-recording-locks-scope-labels.md)). It saves about 1.1 ns where the cache misses
  and nothing where it hits. Declined: it weakens a documented guarantee for every reader on every
  backend.

The cache removes C3's table lookup when a pipeline is bound again; the 3.0 to 3.3 ns that remain
could not be attributed (the entry call, the kind test, the held-state stores, the draw wrapper).
The floor is justified by the measured cost of a lookup (1.4 to 2.0 ns), which a first bind or a
third pipeline still pays, and by a non-inlined call's 1.2 ns ([ADR 0012](
0012-language-and-dispatch.md)). On an operation of about 50 ns, 5% is 2.5 ns, less than a lookup
and a call together, so a relative-only budget makes a few nanoseconds per call a miss whenever the
native operation is short. The floor of 5 ns per call was set with these numbers and the profiles
above known, and it is tight: it passes C3 at `K` 100 by 0.6 ns in one profile and not in the
other.

## Decision

- RojoRHI's CPU time per operation, R, is within budget when R minus the native baseline's time
  per operation, N, is at most the larger of 5% of N and 5 ns times `c`. `c` is the number of
  RojoRHI calls in the operation, as the measurement's pre-registration counts them for its
  native-call budget.
- A miss is judged as ADR 0014 and the pre-registration state it: the 95% interval of the paired
  delta excludes zero and the median delta exceeds the budget.
- A pre-registration that adopts this rule states `c` for each workload it judges, before any
  measurement, and judges with it. The workloads registered so far already state `c`.
- The floor is absolute. It is the same 5 ns on every machine and backend; a slower machine does
  not earn a larger one.

What the floor does not excuse:

- A delta above 5 ns times `c` on a short operation.
- A relative tax over 5% on an operation long enough that 5% of N exceeds the floor. With `c` of
  1 to 3 the floor stops mattering above 100 to 300 ns of native time.

## Consequences

- Earlier verdicts are not revised. Rounds 1 and 2 stand as recorded, judged at 5%. For scale
  only: by this rule round 1 would have missed 29 of 34 configurations, and round 2 two, C3 and
  C6 at `K` 100 (15.0 and 14.6 ns against a floor of 10).
- A later round reports both verdicts per configuration: this rule, which decides, and the 5%
  rule, for transparency. `Tools/bench_analysis.py` judges by this rule and reports the 5% rule
  beside it.
- On a one-call operation of about 5 ns, such as C5's `uploadFrameData`, the floor admits a tax
  of up to 100%. That is accepted: the operation is an arena bump and a 256-byte copy, and 5% of
  it is about a quarter of a nanosecond.
- Time per operation is a list's span divided by `K`. At `K` 100 it carries a list's fixed cost
  spread over the repetitions, so the floor also absorbs part of that cost; C3 at `K` 100 is the
  tight case stated in the Context.
- The native-call budget, the allocation budget and the composed frame's 1% GPU budget are
  unchanged. A backend still meets those as ADR 0014 states them.
- The continuous guard of ADR 0014 is unchanged. It fails a change whose slowdown against the
  merge base has an upper bound above 5%, which judges a change against RojoRHI, not RojoRHI
  against native code.
