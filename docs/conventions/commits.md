# Commit Conventions

**Status**: Accepted

## Messages

- Use `<scope>: <imperative outcome>` in English. Keep the subject at 60 characters when practical,
  never over 72, and omit the trailing period. The scopes are `rhi` for the public interface and
  shared implementation, `metal` for the Metal 4 backend, `imgui` for the optional adapter, `test`,
  `tool`, `build`, `ci` and `docs`.
- Make one explainable behaviour or constraint one commit. Keep its implementation, tests and
  nearby documentation together. Do not split work to manufacture commit count, and do not combine
  unrelated files into a cleanup wave.
- A body, when useful, records the problem, the decision, the tradeoff and a durable reference.
  Validation details belong in the pull request unless they are essential to the decision.
- Commit messages describe engineering outcomes, never execution bookkeeping. Do not mention work
  items, review rounds, backlogs, prompts, agents, models, plans or checkbox completion.
- The author's configured personal identity may appear only in Git author metadata, never in a
  subject, body, trailer, repository content, URL, example or home-directory path. Do not add an AI
  co-author trailer or any other tool identity. The same rule covers pull request titles and
  descriptions: no generated-by footer, no session link, no model or agent name.
- Use local `fixup!` commits while iterating and autosquash them before publication. Published
  `main` contains no WIP or fixup commits.

## Branches and integration

- `main` is protected and buildable. Merge commits and direct pushes are disabled.
- Use one short-lived outcome branch: `feat/<outcome>`, `fix/<outcome>`, `docs/<outcome>` or
  `spike/<question>`. Do not keep `develop`, milestone-wide or release branches.
- Rebase before review. Rebase-merge 2–5 independently useful green commits; squash a single
  logical change or local trial and error. A spike records its result, and a clean implementation
  branch then carries the accepted production work.
- `main` is force-replaced exactly once, when the library's history is imported from the consumer
  it was developed in; that needed explicit owner confirmation at execution time and a verified
  archive tag over the prior `main` beforehand. That import has run. `main` is never rewritten
  again.

## Validation before a commit

- A source change passes formatting, the relevant tests and a build.
- A pull request passes the full build, the CPU contract and validation tests, formatting and the
  policy checks the repository has at that time.
- A change to the interface, the backend or a shader ABI also provides GPU evidence: a run of the
  relevant GPU cases with the platform validation layer enabled. Hosted runners have no Metal 4
  device, so that run happens on Metal 4 Apple Silicon before merge, and hosted CI is limited to
  compiling and inventorying the GPU cases. [testing.md](testing.md) owns the tiers and the
  conformance filter.

## Working across two repositories

RojoRHI is consumed as a submodule, and a change here is driven by a consumer's need.

- An interface change is a `rojo-rhi` branch and pull request of its own. The consumer's branch may
  pin that branch's commit during development.
- Before the consumer's pull request merges, the RojoRHI change is merged, so the pin names a
  commit on `main`. Checking that is the consumer's job, because the consumer alone holds the pin:
  the consumer runs a policy check that its pinned commit is reachable from RojoRHI's `main`.
  RojoRHI has no such check and cannot have one — it does not know which commit any consumer pins.
- RojoRHI changes meet RojoRHI's conventions and conformance tests. The consumer's submodule bump
  meets the consumer's own suite.
- A consumer commit never edits files under its `RojoRHI/` mount. A change that looks like a
  one-line fix in the mount is still a RojoRHI pull request.

## Differences from Luminex

- **Scopes.** Luminex's list covers its renderer, scene, asset, editor and app layers. RojoRHI
  keeps only the scopes it has units for and adds `imgui` for the optional adapter, which is a
  separate target here rather than one target among many.
- **`main` is replaced once.** Luminex's `main` is never force-pushed after its baseline cutover.
  RojoRHI's `main` is force-replaced exactly once, at the code import, because the repository
  starts with placeholder history that the imported history must replace wholesale. The rule is
  otherwise identical: never rewritten afterwards.
- **Two-repository workflow.** Luminex has no submodule-pin rule to state. RojoRHI adds the section
  above, because a change here and the consumer's bump are two pull requests with an ordering
  between them.
- **Evidence.** Luminex additionally requires image and performance evidence for rendered-output
  changes. RojoRHI renders no images of its own, so its evidence is the test tiers and a
  validation-clean GPU run.
