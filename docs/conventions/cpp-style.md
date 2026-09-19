# C++ Style

**Status**: Accepted

Formatting is owned by a `.clang-format` file that arrives with the code import; this file covers
what a formatter cannot. Names are owned by [naming.md](naming.md).

- **C++23.** Prefer `std::expected`, `std::span`, `std::string_view`, `std::format` and ranges. No
  RTTI-dependent design. Exceptions only cross third-party boundaries and never leave the public
  interface.
- **Files.** One primary type per header, `#pragma once`, and the file envelope below at the first
  physical line of every hand-written `.h` and `.cpp` the repository owns.
- **Includes.** Own header first, then RojoRHI headers, then third-party, then standard library,
  with a blank line between groups. Public headers use `<rojoRHI/...>`; a private header inside the
  library is included by the path its owner uses.
- **Errors.** Creation and loading boundaries return `Result<T>`, the library's single
  `std::expected` alias, carrying either the value or an `Error` with a stable `ErrorCode` and a
  diagnostic message. Misuse of the interface is not an error code: it is a fatal `ROJORHI_ASSERT`,
  because a caller violating a precondition has a bug rather than a condition to recover from.
  Never swallow a failure silently.
- **Labels.** Every GPU object is created with a debug label, defaulted when the caller gives none,
  so a capture names what it shows.
- **Public headers are dependency-free.** A public header names no backend, windowing, ImGui or
  third-party type; only the standard library and RojoRHI's own types appear
  ([ADR 0003](../decisions/0003-thin-rhi.md)). Each public header must compile standalone as the
  only include of an otherwise empty translation unit, built with a dependency-free command line.
  The check that enforces this arrives with the code import.
- **Experimental code** stays off `main` on a short-lived `exp/<topic>` branch. Only conclusions,
  ADRs and adopted production code return.

## The private base

`Source/Base/` is the only home of assertions, alignment helpers, JSON escaping and logging. It is
private: no public header includes it, and no consumer can reach it.

- Logging is three macros — `ROJORHI_LOG_INFO`, `ROJORHI_LOG_WARN` and `ROJORHI_LOG_ERROR` — that
  format with `std::format` and hand severity plus the formatted message to one public message
  callback. When no callback is installed, messages go to stderr. RojoRHI takes no logging
  dependency and knows nothing about a consumer's log sinks.
- `ROJORHI_ASSERT(cond, msg)` logs the failed condition, file, line and message at error severity,
  then aborts. It is unconditional: a release build asserts too, because a violated precondition
  has no correct continuation.
- Nothing else joins the base. A helper with one caller stays with that caller.

## File headers

Every hand-written `.h` and `.cpp` starts with this four-line envelope. Both ruler lines are exactly
120 ASCII characters (`//` followed by 118 `-` characters):

```cpp
//----------------------------------------------------------------------------------------------------------------------
/// @file Device.h
/// @brief Declares the device, its creation surface, and per-pass GPU timings.
//----------------------------------------------------------------------------------------------------------------------
```

Match `@file` to the basename exactly and give `@brief` a concise, file-specific summary. No
copyright notice, generated-history note or blank line sits above the envelope. The `-` ruler
belongs only to these two lines. Fetched and third-party code is outside this rule.

## Function boundaries in implementation files

Every hand-written named function definition in a `.cpp` file the repository owns starts with a
separator whose physical line is exactly 120 characters: `//` followed by 118 `=` characters.

```cpp
//======================================================================================================================
```

- Source stays at the formatter's 100-column limit; the separator is the only routine exception.
- Put one blank line before the separator, and the separator before the earliest token of the
  definition, including an attribute, `template` or `requires` clause. Function-specific rationale
  may sit between the separator and the signature with no blank line between.
- It applies to free and anonymous-namespace functions, out-of-line members, constructors,
  destructors, conversions, operators, templates and specializations, `main`, and out-of-class
  `= default` or `= delete` definitions. Each overload gets its own separator.
- Avoid in-class definitions in `.cpp` files. If an unavoidable local class defines one, indent the
  separator with the function and reduce its `=` padding so the line stays exactly 120 characters.
- Do not add separators to declarations, explicit instantiations, lambdas, control-flow blocks or
  function-like macro definitions. Catch2 `TEST_CASE` and related macros are test-unit boundaries
  and take the canonical separator.
- The `=` ruler belongs only to function and test-unit boundaries, the `-` ruler only to the file
  envelope. Do not invent titled or alternate ruler styles.

A Clang-based check rejects missing, duplicate, malformed and orphan separators using the
compilation database rather than a regular expression guessing at C++ definitions. It arrives with
the code import.

## Comments

- Public and protected declarations in public headers use Doxygen `///`. Begin with a concise
  summary and document ownership, lifetime, threading, valid ranges, units, coordinate and colour
  spaces, failure behaviour and externally visible side effects when relevant. Use `///<` for a
  short field or enumerator contract and `@copydoc`/`@inheritdoc` when an override inherits its
  base contract. Backend and implementation headers are not public API.
- Private declarations need no Doxygen and should not carry comments restating their names.
- Implementation comments are sparse: GPU state and synchronization invariants, cross-frame
  ownership, non-obvious math or space conventions, documented driver workarounds, performance
  tradeoffs, or why an apparently redundant operation is required. Put each next to the smallest
  block it explains.
- Long functions may use short noun-phrase phase labels such as `// Residency commit`. Do not
  number or decorate them.
- Never record work items, review rounds, prompts, orchestration, commit hashes, source line
  numbers or implementation chronology. Do not write `Step 1`, `Next`, `for now` or `as requested`.
- Keep no commented-out code and no bare `TODO`/`FIXME`. A tracked exception is
  `TODO(<issue-id>): <action and reason>` describing real remaining work.

## Differences from Luminex

- **No shared Core library.** Luminex's RHI includes `Core/Assert.h`, `Core/Log.h`, `Core/Align.h`
  and `Core/Json.h` from a library shared with the renderer. RojoRHI owns a private `Source/Base/`
  instead, so the library links nothing outside its own tree and a consumer inherits no base
  library it did not ask for.
- **Logging.** Luminex logs through spdlog macros. RojoRHI formats with `std::format` and emits
  through one public message callback, defaulting to stderr, because a library must not choose its
  consumer's log sink ([ADR 0003](../decisions/0003-thin-rhi.md)).
- **One error domain.** Luminex picks the owning domain's alias at each boundary (`rhi::Result<T>`,
  `engine::AssetResult<T>`). RojoRHI has one domain, so `Result<T>` is the only alias.
- **Dependency-free headers are the rule, not an exception.** Luminex holds its public RHI headers
  to a stricter standalone command line than the rest of its tree. Here that stricter rule covers
  every public header, since all of them are the interface.
- **No shader style rules.** Luminex's production shader conventions do not carry over; RojoRHI's
  only shaders are test oracles, covered by [testing.md](testing.md).
