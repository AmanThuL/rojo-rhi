# Extracting RojoRHI from Luminex

**Status**: Proposed

R2.1 to R2.4 are the consumer's four steps towards the extraction, named here and in
`Tools/Rename/substitution.tsv` only: R2.1 writes these founding documents, R2.2 decouples the
component in place, R2.3 runs the mechanical rename below, and R2.4 extracts the component into
this repository and mounts it back as a submodule.

This guide explains the mechanical substitution that R2.3 applies to move Luminex's `RHI/`
component to the RojoRHI identity, before R2.4 extracts it into this repository as a submodule.
The ordered rules live in `Tools/Rename/substitution.tsv`, tab-separated, applied top to bottom;
this guide records why each rule exists, what the dry run against Luminex found, what the rules
deliberately do not touch, and what still needs a hand edit.

## Column vocabulary

`substitution.tsv` has one header row and six columns, split on the tab character with no quoting
(Python's `csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)`, or equivalently
`line.rstrip("\n").split("\t")`). A `"` anywhere in a field, including the leading anchor of rows
41, 42, 45, 70, 90 and 100 and the include delimiters row 40 matches and emits, is ordinary field
content, never a quote character — a reader that applies the default quoting dialect to a
tab-separated file will treat that `"` as an open quote and merge it with the next field. The
columns are:

- `order` — an integer; rows apply low to high, so a later row never undoes an earlier one.
- `kind` — `path` renames a file or directory; `regex` rewrites file contents with a Python `re`
  pattern. R2.3 applies every `regex` row with Python's `re` module; `\b` and `\t` carry `re`'s
  meaning (a zero-width word boundary and a literal tab character, respectively, never a shell or
  POSIX-bracket-expression escape). The dry-run counts below are reproducible with any engine that
  shares `re`'s `\b`/`\t` semantics and greedy alternation; they were counted with Python 3's `re`
  module, called from the counting scripts referenced in each section below.
- `scope` — `component` means files under the component directory (`RHI/` before R2.3 runs);
  `consumers` means the specific outside paths named in the row's `note`; `all` means both.
- `pattern` / `replacement` — the regex or literal path and what it becomes. A `|` inside a
  pattern is a literal alternation operator, not an escaped table-rendering character.
- `note` — rationale, or, for the rows restricted to a named file set, that file set spelled out in
  plain words.

## Identity being introduced

The substitution moves Luminex's component to the identity fixed by the owner: namespace
`rojoRHI` and `rojoRHI::metal4`; public includes under `<rojoRHI/...>`; macros `ROJORHI_ASSERT`
and `ROJORHI_LOG_*`; default GPU label prefix `rojorhi.`; build targets `RojoRHI` and
`RojoRHIMetal4ImGui`; directory `RojoRHI/`. This mixed-case spelling is the one approved exception
to the project's lowercase namespace and include-root convention, kept because RHI is already
written as an initialism and the mixed case preserves the word boundary.

## Dry-run counts (Luminex, at R2.1, 2026-09-19)

Measured against Luminex `main` at the commit R2.1 started from. R2.3 re-derives these after R2.2
moves tests in, since file locations shift before the rename runs.

| row | pattern (regex rows) | inside component: matches / files | outside component: matches / files |
|---|---|---|---|
| 30 | `\blmx::rhi\b` | 90 / 44 | 382 / 67 (`Source Tests Tools Benchmarks xmake.lua`) |
| 31 | `(?<![\w:])rhi::` | 4 / 2 (prose comments only; the row is consumers-scoped and leaves them) | 1,916 / 161 (`Source Tests Benchmarks`, `*.h` `*.cpp` `*.mm`) |
| 40 | `(#[ \t]*include[ \t]+)"RHI/([^"]+)"` | 99 / 34 | 68 / 57 (`Source Tests Tools Benchmarks`) |
| 41 | `"RHI/(Format\.h\|TextureDesc\.h)"` | n/a (consumers-only row) | 4 / 1 (`Tools/module_contract.json`) |
| 42 | `"RHI/\*\*/\*\.h"` | n/a (consumers-only row) | 1 / 1 (`Tools/check_rhi_headers.py`) |
| 45 | `"RHI/` | n/a (consumers-only row) | 13 / 5, counted after row 41 has already run (`xmake.lua`, `xmake/tasks.lua`, `Tools/check_project_policy.py`, `Tools/check_cpp_comments.py`, `Tools/module_contract.json`) |
| 50 | `\bLMX_ASSERT\b` | 146 / 11 | n/a (component-only row) |
| 60 | `\bLMX_LOG_(TRACE\|DEBUG\|INFO\|WARN\|ERROR)\b` | 20 / 6 | n/a (component-only row) |
| 70 | `"lmx\.` | 26 / 9 | n/a (component-only row) |
| 80 | `lmx\.(device\.frameData\|imgui\.formatCarrier)` | n/a (consumers-only row) | 60 / 8 (`Tests/CaptureTests.cpp`, `Tests/CaptureSchemaTests.cpp`, `Tools/GpuDebug/**`) |
| 90 | `"RHIMetal4ImGui"` | 1 / 1 (`RHI/Backends/Metal4/ImGui/xmake.lua`) | 4 / 2 (`Source/App/xmake.lua`, `Tools/module_contract.json`) |
| 100 | `"RHI"` | 2 / 2 (`RHI/xmake.lua`, `RHI/Backends/Metal4/ImGui/xmake.lua`) | 40 / 11 (six remaining `xmake.lua` files and `Tools/module_contract.json`, 18/7, plus `Tools/check_cpp_layout.py` 1/1, `Tools/check_cpp_comments.py` 2/1, `Tools/check_rhi_headers.py` 1/1 and `Tools/tests/test_module_deps.py` 18/1) |

Rows 10 and 20 are `path` rows; they rename the component's public include root and then the
component directory itself, so their effect is one move each, not a match count.

Row 100's file list is intentionally narrow: its bare `"RHI"` pattern also matches unrelated
quoted strings elsewhere in the tree (for example inside comments, prose or unrelated identifiers),
so the row is scoped to only the files checked below. Every `"RHI"` occurrence actually found in
those files — `xmake.lua` files' `add_deps`/`target()` calls, `module_contract.json`'s `roots` and
`targets` entries and its per-unit `targets` lists, `check_cpp_layout.py`'s `CPP_ROOTS` tuple,
`check_cpp_comments.py`'s `SOURCE_ROOTS` tuple and its `module = "RHI"` fallback,
`check_rhi_headers.py`'s `ROOT / "RHI" / "Include"` path segment, and `test_module_deps.py`'s
dictionary keys and target-name list entries in its `INCLUDE_CONTRACT`/target-dump fixtures —
names the `RHI` build target, the `RHI` unit/root entry, or the `RHI` path segment that row 20
renames the directory to. None of them is an unrelated string that row 100 would need to skip; the
narrow file list exists to keep it that way, not because a false hit was found and excluded.

## Ordering rationale for rows 40–45

Three different rows rewrite text that starts `"RHI/` (in quotes) or `#include "RHI/`, because
`RHI/` is spelled with two different meanings depending on where it appears, and each meaning needs
a different case:

- **As the public include root** (row 10's target, `RHI/Include/RHI` → `RHI/Include/rojoRHI`): an
  `#include "RHI/Device.h"` directive, and the equivalent spellings that name the same header
  without the `#include` keyword — `module_contract.json`'s `"headers": ["RHI/Format.h",
  "RHI/TextureDesc.h"]` entries (the `asset` unit's allowed extra headers, spelled the way a
  consumer includes them) and `check_rhi_headers.py`'s `PUBLIC_INCLUDE.glob("RHI/**/*.h")` (glob
  anchored at `RHI/Include`, so `RHI/` here again names the public include root, not the component
  directory). All three become `rojoRHI/...`, lower-case first letter, matching row 10. Row 40
  handles the `#include` form; rows 41 and 42 handle the two non-`#include` forms, where the
  surrounding quotes belong to JSON and Python string literals rather than to an include directive
  and therefore stay as they are.
- **As the component directory** (row 20's target, `RHI` → `RojoRHI`): build- and tool-file paths
  such as `includes("RHI/xmake.lua", ...)` in the root `xmake.lua`, `os.files("RHI/**.h")` in
  `xmake/tasks.lua`, the `"RHI/"` entry in `check_project_policy.py`'s `PROCESS_ROOTS`, and
  `check_cpp_comments.py`'s `root / "RHI/Include"` / `root / "RHI/Backends/Metal4/ImGui/Include"`
  path joins, plus `module_contract.json`'s own `"paths": ["RHI/Include"]`-style unit paths. All of
  these become `RojoRHI/...`, matching row 20. Row 45 handles this case.

Row 45's pattern (`"RHI/`) is a strict textual superset of rows 41 and 42's patterns
(`"RHI/Format.h"`, `"RHI/TextureDesc.h"`, `"RHI/**/*.h"`, all quoted immediately after `RHI/`), and
`module_contract.json` contains both meanings in one file. Row 45 must therefore run after rows 41
and 42: once they have rewritten the four header-list entries to `"rojoRHI/Format.h"` and
`"rojoRHI/TextureDesc.h"`, those strings start with a lower-case `r` and no longer match row 45's
capital-`R` pattern, so row 45 only ever touches the remaining `"RHI/Include"`-style directory
paths. Running row 45 first would instead capitalise the header entries to `"RojoRHI/Format.h"`,
which violates the include-spelling rule that a public include root must be spelled exactly
`rojoRHI/`. The three rows never collide with row 40, because row 40's pattern requires the
`#include` keyword immediately before the opening quote, which none of rows 41, 42 or 45 match.
Row 40 additionally consumes the closing quote of the directive it rewrites, so after it has run an
include line no longer contains `"RHI/` at all, and the ordering between row 40 and rows 41, 42 and
45 holds for the same reason it did before.

Row 40 also changes the delimiter. Luminex includes its RHI headers in quoted project form
(`#include "RHI/Device.h"`), while ADR 0001's call-site example and
[naming.md](../conventions/naming.md) both fix the post-rename form as an angle include
(`#include <rojoRHI/Device.h>`), which is what a consumer mounting the library sees. Row 40
therefore matches the whole directive and emits the angle form, in the component and in consumers
alike. No Luminex include of the RHI root uses angle brackets today — searching every tracked file
under `RHI`, `Source`, `Tests`, `Tools` and `Benchmarks` for `#include <RHI/` returns nothing — so
no second row is needed for an already-angled form, and the row's dry-run counts are unchanged from
the quote-preserving version it replaces. The formatter groups includes by delimiter, so moving a
directive from the quoted group to the angled one can leave a file's include block out of the
project's preferred order; reordering include groups is left to the separate formatting commit that
follows R2.3, not expressed as a substitution row.

## Forms the table rows miss

These checks were run beyond the rows above. Two of them found forms that needed rows of their own:
build and tooling paths gained rows 41, 42 and 45, and namespace-relative references gained row 31.
The rest are recorded so the next reader does not repeat them.

- **Build and tooling paths naming the component** — searching every `xmake.lua`, `xmake/*.lua`
  and `Tools/*.py`/`Tools/tests/*.py` file for quoted `"RHI"` and `"RHI/...` strings finds sites the
  original ten rows missed entirely: the root `xmake.lua`'s `includes("RHI/xmake.lua", ...)` line,
  `xmake/tasks.lua`'s `os.files("RHI/**.h")`/`"RHI/**.cpp"` format globs,
  `check_project_policy.py`'s `PROCESS_ROOTS` entry `"RHI/"`, `check_cpp_layout.py`'s
  `CPP_ROOTS` entry `"RHI"`,
  `check_cpp_comments.py`'s `SOURCE_ROOTS` entry `"RHI"`, its `module = "RHI"` fallback and its two
  `root / "RHI/..."` path joins, `check_rhi_headers.py`'s `"RHI"` path segment and `"RHI/**/*.h"`
  glob, `module_contract.json`'s remaining directory-path entries beyond the two already known
  `"headers"` entries, and `test_module_deps.py`'s dictionary keys and target-list entries that
  reuse the bare identity `"RHI"`. Rows 41, 42, 45 and the widened row 100 above cover these; the
  "Ordering rationale" section explains why three separate rows were needed rather than one.
  `check_rhi_headers.py` in particular is a file R2.2 is expected to move under the component and
  whose four `rhi-*` `module_contract.json` units R2.2 is expected to replace; R2.3 re-derives rows
  41, 42 and 100's file list against the tree it actually finds rather than assuming these exact
  paths still exist.

- **Namespace-relative `rhi::` references** — Luminex writes most of its references to the
  component from inside a `namespace lmx::…` block, where the enclosing `lmx` makes the bare
  spelling `rhi::Device` resolve to `lmx::rhi::Device`. `Source/App/EditorShell.cpp` takes
  `rhi::Device&` parameters inside `namespace lmx::app`, for one. There are 1,916 such references
  in 161 files across `Source`, `Tests` and `Benchmarks`, and row 30's `\blmx::rhi\b` matches none
  of them, because none spells `lmx::` at the call site. Left alone they would resolve to nothing
  after row 30 empties `lmx::rhi`, so nothing would compile. Row 31 rewrites them to `rojoRHI::`.
  Its `(?<![\w:])` lookbehind is a Python `re` fixed-width lookbehind: it leaves an identifier that
  merely ends in `rhi` alone, and it leaves any already-qualified `rhi::` alone, so a hypothetical
  `std::rhi::x` is untouched. The row runs after row 30, so the fully qualified form is already
  `rojoRHI::` — which the lookbehind cannot match either, the initialism being upper-case there.
- **Split namespace aliases** — `grep -rnE 'namespace rhi\b' RHI Source Tests` finds nine test
  files (`Tests/GpuVisibilityRailTests.cpp`, `Tests/EngineAssetTests.cpp`,
  `Tests/GpuVisibilityTests.cpp`, `Tests/GpuVisibilityContributionTests.cpp`,
  `Tests/GpuVisibilityWorkTests.cpp`, `Tests/GpuLightClusterTests.cpp`,
  `Tests/GpuSceneTableTests.cpp`, `Tests/GpuLightDebugViewTests.cpp`,
  `Tests/EngineSceneTestSupport.h`) each declaring `namespace rhi = lmx::rhi;`. Row 30 rewrites the
  right-hand side of that declaration, producing `namespace rhi = rojoRHI;`. Row 31 is safe in
  these nine files too: the declaration contains no `rhi::`, so the row cannot touch it, and the
  uses that the alias served are rewritten to `rojoRHI::` like every other namespace-relative
  reference. The alias line that survives is valid C++ and no longer used by anything; C++ has no
  unused-alias diagnostic, so it neither breaks nor warns. Removing the nine now-dead lines is
  listed under hand edits rather than forced through a row, because deleting a whole line is not
  what a substitution table expresses.
- **No compatibility alias.** A `namespace lmx::rhi = rojoRHI;` shim in the consumer would let rows
  30 and 31 be skipped and keep every stale reference compiling. It is rejected: ADR 0001 fixes one
  spelling precisely so that a reference to the old identity fails loudly instead of resolving by
  accident, and a shim would hide exactly the references R2.3 exists to find.
- **`RHI/` inside prose comments** — outside `#include` directives, four comments still spell the
  old path: `RHI/Include/RHI/Texture.h` (mentions `RHI/Validate.h`),
  `RHI/Backends/Metal4/Source/Metal4FrameArena.h` (mentions `RHI/Metal4/Metal4FrameData.h`),
  `Benchmarks/FrameData/Runner.h` (mentions `RHI/Include/RHI/Metal4/Metal4FrameData.h`), and
  `Benchmarks/FrameData/DeliverPerDrawData.h` (mentions the public `RHI/Include` surface). None of
  the rows above rewrite comment text, and a generic comment-matching regex risks touching
  unrelated prose. These four are listed under "Hand edits deferred past R2.3" below.
- **Python/JSON consumers of component-emitted `lmx.` strings** — the search

  ```
  grep -rnE 'lmx\.(device|queue|imgui|temporal\.scaler|pass\.temporal\.scaler|[a-z]+\.(placed\.)?unnamed)' Source Tests Tools docs
  ```

  finds only the `lmx.device.frameData` and `lmx.imgui.formatCarrier` sites row 80 already covers
  (`Tests/CaptureTests.cpp`, `Tests/CaptureSchemaTests.cpp`, and, under `Tools/GpuDebug/**`,
  `uniformlib.py` and its `tests/test_anomalylib.py`, `test_bundlelib.py`, `test_schemalib.py`,
  `test_gputrace_dump.py`, `test_uniformlib.py`). The one other match, a label example in a
  Luminex planning document, is a planning artifact, not a consumer, and is out of scope.

## Exclusions

The substitution deliberately does not touch:

- **Renderer-owned `lmx.*` labels.** Row 70 rewrites only the component's own default and
  device-owned label prefix (`"lmx.` inside `RHI/`). Labels the renderer or scene layers author
  for their own passes and resources keep the `lmx.` prefix; they are not the component's identity
  to rename.
- **Every `LMX_*` environment variable.** The component reads no environment variable at all:
  there is no `getenv` call anywhere under `RHI/`. `LMX_CAPTURE_PATH` appears once, inside a
  capture-failure message it prints, and `LMX_MAX_FRAMES` appears once, in a comment showing an
  example command line. Both stay exactly as spelled, because the process environment they name is
  owned by whoever launches the binary — Luminex's App and its automation — and the component only
  mentions those names in text.
- **The `lmx` namespace outside the component.** Row 30 rewrites `lmx::rhi` and its nested
  `lmx::rhi::metal4`, never bare `lmx::` used by Core, Render, Asset, Scene or App.
- **Luminex `docs/` prose.** Any `docs/` file in Luminex that mentions `RHI/` is out of scope of
  this substitution; Luminex's own R2.4 slice updates such documents by hand.

## Hand edits deferred past R2.3

These sites are known before the rename runs but are not expressed as substitution rows, because
each needs judgement a mechanical regex should not attempt.

**The environment-variable message:**

- `RHI/Backends/Metal4/Source/Metal4Capture.cpp` — the user-facing capture-failure message names
  `LMX_CAPTURE_PATH` in its text (`"Output path must end in .gputrace. Set LMX_CAPTURE_PATH and
  relaunch."`). The variable itself is excluded (see above); this is a candidate for the same
  message to instead print whatever name the consumer that owns the variable uses, decided when
  the message's owner is known post-extraction.

**Namespace-relative spellings row 31 does not reach:**

- `RHI/Backends/Metal4/Source/Metal4Resources.h` (three sites) and
  `RHI/Backends/Metal4/Source/Metal4CommandList.cpp` (one) — prose comments inside the component
  naming interface symbols as `rhi::Heap`, `rhi::bytesPerPixel`, `rhi::validateTextureView` and
  `rhi::CommandList::textureBarrier`. These are the only four `rhi::` matches inside `RHI/`, and row
  31 is consumers-scoped so it leaves them; they read as `rojoRHI::` once edited by hand.
- The nine test files listed above keep a `namespace rhi = rojoRHI;` line that nothing uses after
  row 31 runs. The line is valid and silent, so it blocks nothing; deleting it is a one-line edit
  per file, not a substitution.
- `Tools/GpuDebug/xctracelib.py` names `rhi::RenderPassDesc` in a module docstring. It is a Python
  tool, outside row 31's file scope and outside any compile, so it is corrected by hand with the
  other prose.

**Consumers of the component directory and target name outside `#include`:**

- `RHI/Include/RHI/Texture.h:29` and `RHI/Backends/Metal4/Source/Metal4FrameArena.h:30` — prose
  comments naming the pre-rename path (`RHI/Validate.h`, `RHI/Metal4/Metal4FrameData.h`).
- `Benchmarks/FrameData/Runner.h:48` and `Benchmarks/FrameData/DeliverPerDrawData.h:20` — prose
  comments naming the pre-rename public include surface (`RHI/Include/RHI/Metal4/Metal4FrameData.h`,
  `RHI/Include`).
- `Tools/tests/test_module_deps.py`'s 21 `"RHI/..."` fixture strings across its `INCLUDE_CONTRACT`,
  `INCLUDE_TREE` and `INCLUDE_DIRS` test data (for example `RHI/Source/Device.cpp`,
  `RHI/Include/RHI/Format.h`, `RHI/Format.h`, `RHI/Device.h`). Unlike `module_contract.json`, this
  file mixes the two `RHI/` meanings inconsistently: some entries are single-segment component-
  directory paths (`RHI/Backends`, matching row 45's case), others are the doubly-nested
  `RHI/Include/RHI/...` shape (needing both a row-45-style and a row-41-style rewrite on the same
  string, at different offsets), and others still are bare invented header names — `RHI/Format.h`
  and `RHI/Device.h` — that only coincidentally share a name with the real `headers` list entries
  row 41 targets; `Device.h` is not one of the component's real public headers. No single regex, or
  small fixed set of them, can separate these roles reliably in one already-fictional test fixture,
  so they are left for a hand edit once the fixture is next touched. The file's bare `"RHI"`
  identity tokens (dictionary keys and target-list entries) are still safely covered by the widened
  row 100, since that pattern only ever matches the exact standalone token.

## After R2.3: rebase and force-replace `main`

This guide and its substitution table are authored on the orphan `foundations` branch, which holds
none of the placeholder history this repository started with. R2.4 rebases `foundations` onto the
history imported from Luminex, then force-replaces `main` with the result once the owner confirms
the replacement and the archive tag over the prior `main` has been verified. That destructive step
happens exactly once.
