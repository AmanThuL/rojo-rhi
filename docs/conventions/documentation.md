# Documentation Conventions

**Status**: Accepted

Documentation has one owner for each kind of fact. A newer lower-precedence document does not
silently override an accepted higher-precedence decision.

## Types and precedence

1. **ADR** (`docs/decisions/`): one durable architectural decision. An accepted ADR is immutable;
   change it with a superseding ADR.
2. **Convention** (`docs/conventions/`): enforceable repository-wide behaviour. Update it together
   with the code or tooling that makes the rule true.
3. **Architecture or guide** (`docs/architecture/`, `docs/guides/`): the current shape of the
   library and the operator workflows around it. These describe the repository as it exists now.
4. **Active plan** (`docs/plans/`): an accepted change currently being executed. It may hold
   sequencing and exit criteria, but it never becomes a permanent dependency of source comments.

`README.md` and `AGENTS.md` are navigation and operation surfaces. They summarize; they introduce
no architecture decision of their own.

RojoRHI documents stand alone: a reader with no consumer checkout can follow them. An ADR that
restates a decision first accepted in a consuming project names that decision as provenance by
number and title, in a `**Restates**:` line, and never by a link into another repository.

## What documents may not claim

- A second backend is a design target, not a capability. No document, release text or interface
  comment describes RojoRHI as having one.
- A tool, check or command that does not exist yet is written as arriving with the work that adds
  it, never in the present tense.
- The repository is public and licensed under the Apache License, Version 2.0; see `LICENSE`.
  Nothing here describes a package registry entry or a published release, since neither exists.
- `README.md` is the public-facing surface: it names no unimplemented backend as a current
  capability and no internal milestone or `M<number>` language, a rule
  `Tools/check_project_policy.py` enforces mechanically.

## Status lifecycle

Every file under `docs/` carries one explicit `**Status**:` field within its first 30 lines.

- `Proposed`: open for decision; not binding.
- `Accepted`: binding and current.
- `In progress`: the single accepted plan being executed.
- `Implemented`: shipped current behaviour; update it when the behaviour changes.
- `Superseded by <link>`: replaced, retained only for provenance.

At most one file under `docs/plans/` may be `In progress`; zero means nothing is in execution. When
a plan closes, extract its durable decisions to ADRs and its current behaviour to an architecture
page or guide, then remove the plan from the published baseline.

## Writing and links

- State the fact first, then rationale and evidence. Use present tense for current behaviour.
- Link to symbols, documents, specifications or stable upstream sources. Do not cite source line
  numbers, commit hashes, review rounds or task identifiers as explanations.
- Use repository-relative paths in files and examples. Never store a personal home-directory path.
- Prose wraps at 100 columns; tables and code blocks are exempt.
- Line budgets: `README.md` and `AGENTS.md` at most 250 lines; each convention, architecture page,
  guide and the active plan at most 300. These are line budgets, not word limits.
- Keep raw captures, temporary measurements and recovery bundles outside the published tree.

## Differences from Luminex

- **Fewer types.** Luminex also has roadmap, milestone, postmortem and research documents. RojoRHI
  has none of them: scope and sequencing are owned by the consumer's roadmap until RojoRHI has its
  own, and shipped-behaviour records belong to the consumer that ships frames. The precedence list
  is correspondingly shorter, and the statuses those types carried — `Frozen — non-normative` and
  `Closed` — are not used here.
- **Provenance instead of links.** Luminex's documents link each other freely. A RojoRHI document
  cannot link into a consumer's tree, because a reader may not have one, so a restated decision
  names its origin by number and title instead.
- **A narrower public surface.** Luminex governs what its README, release text and gallery captions
  may claim about the renderer. RojoRHI has one public document, `README.md`, held to the same
  no-unimplemented-backend and no-milestone-language rule; every other document here is engineering
  record, not release copy.
- **A stated wrap width.** Luminex wraps documentation prose at 100 columns in practice but states
  no rule for it. RojoRHI states the rule, with tables and code blocks exempt, so the practice is
  checkable rather than inferred from the files that happen to follow it.
