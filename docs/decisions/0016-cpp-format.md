# ADR 0016: C++ format follows the No Graphics API style

**Status**: Accepted (2026-10-12)

## Context

The v2 interface follows Sebastian Aaltonen's "No Graphics API" model, and its reference code and
examples read next to that project's sources. The current `.clang-format` is LLVM-based with a
100-column limit and unindented namespaces. A formatting change touches every C++ file, so it is
cheap exactly once: before any v2 code exists, when new code can be born in the new style and the
existing code reformatted in a single commit.

## Decision

The C++ format moves to the style No Graphics API uses: Microsoft base style, 160 columns,
indented namespace contents, one argument per line once wrapped with the closing bracket on its own
line, and left-aligned pointers and references. The style settings are NoGraphicsAPI's upstream
`.clang-format` (github.com/sebbbi/NoGraphicsAPI), adopted verbatim:

```yaml
BasedOnStyle: Microsoft
ColumnLimit: 160
IndentWidth: 4
NamespaceIndentation: All
AlignAfterOpenBracket: BlockIndent
BinPackArguments: false
BinPackParameters: false
PointerAlignment: Left
ReferenceAlignment: Left
BraceWrapping:
  AfterNamespace: false
AllowShortCaseLabelsOnASingleLine: true
Cpp11BracedListStyle: false
SpaceAfterTemplateKeyword: false
```

This block is not the complete file. The repository's non-style settings are retained alongside
it: `Standard: Latest`, `InsertNewlineAtEOF: true`, `SortIncludes: CaseSensitive` and
`IncludeBlocks: Preserve`.

The decision is made now. It is applied as the first build commit of the Null-backend stage, before
any v2 code exists. That commit changes `.clang-format`, reformats the existing C++ once, and
updates in the same change the C++ style convention and the layout policy tool
(`Tools/check_cpp_layout.py`) that encode the old layout, so that the tooling and the convention
describe the code they check.

## Consequences

- Until that commit lands, `.clang-format` keeps its current settings, and new code follows the
  convention as written today. This ADR does not change either.
- The reformatting commit touches every C++ file and is reviewed as a mechanical change; later
  commits do not mix formatting with behaviour.
- The 100-column wrap for documentation prose is unaffected: this decision covers C++ source only.
