#!/usr/bin/env python3
"""Check repository documentation, identity, and change-message policy.

Every path is relative to the component root, and every git call runs there, so the answers are
the same inside a host checkout and in the component's own repository.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
SKIP_PARTS = {".git", "build", "ThirdParty"}
SKIP_PREFIXES = (("Assets", "Fetched"),)
STATUS_DIRS = {
    "architecture", "conventions", "decisions", "guides", "milestones", "plans",
    "postmortems", "research", "roadmap", "specs",
}
LINE_BUDGETS = {"AGENTS.md": 250, "README.md": 250, "docs/": 300}
HOME_PATH = re.compile(
    r"(?i)(?:/" + r"Users/[^/\s`]+|/" + r"home/[^/\s`]+|[A-Z]:\\Users\\[^\\\s`]+)"
)
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
STATUS_FIELD = re.compile(
    r"(?im)^\s*(?:[-*]\s*)?(?:\*\*)?status(?:\*\*)?\s*:(?:\*\*)?\s*(.+)$"
)
STATUS_PREFIXES = ("proposed", "accepted", "in progress", "implemented", "frozen", "closed")
PROCESS_PATTERNS = (
    re.compile(r"(?i)\b(?:m\d+(?:\.\d+)?\s+)?task\s*#?\d+\b"),
    re.compile(r"(?i)\breview(?:er)?[- ](?:finding|request|round)\b"),
    re.compile(r"(?i)\b(?:prompt|agent|subagent|backlog)\b"),
    re.compile(r"(?i)\b(?:opus|sonnet|haiku|fable)\b"),
    re.compile(r"(?i)\b(?:work[- ]item|plan[- ](?:checkbox|completion|amendment))\b"),
    re.compile(r"(?i)\b(?:as requested|per the prompt|controller resolution)\b"),
)
PROCESS_ROOTS = (
    "xmake/", "Include/", "Source/", "Backends/", "Shaders/", "Tests/", "Tools/ImGuiBufferProbe/",
)
COMMIT_SUBJECT = re.compile(
    r"^(?:rhi|metal|render|shader|scene|asset|engine|editor|app|core|tool|build|ci|docs|test|spike): "
    r"[a-z0-9]"
)
NON_IMPERATIVE_START = re.compile(
    r"(?i)^[^:]+:\s+(?:added|changed|created|documented|fixed|implemented|made|moved|"
    r"refactored|removed|renamed|updated)\b"
)
PUBLIC_COPY = {Path("README.md")}
PUBLIC_PLANNING_LANGUAGE = re.compile(r"(?i)\bmilestones?\b|\bM\d+(?:\.\d+)?\b")
PUBLIC_UNSHIPPED_BACKEND = re.compile(r"(?i)\b(?:D3D12|Direct3D\s*12|Vulkan)\b")


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout


def repository_files() -> list[Path]:
    names = git("ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0")
    files: list[Path] = []
    for name in names:
        if not name:
            continue
        path = Path(name)
        if any(part in SKIP_PARTS for part in path.parts):
            continue
        if any(path.parts[: len(prefix)] == prefix for prefix in SKIP_PREFIXES):
            continue
        full = ROOT / path
        if full.is_file():
            files.append(path)
    return sorted(files)


def read_text(path: Path) -> str | None:
    full = ROOT / path
    data = full.read_bytes()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def author_identity_tokens() -> set[str]:
    """Derive private deny tokens in memory; never serialize author metadata."""
    raw = git("log", "--all", "--format=%an%x00%ae%x00")
    fields = [field.strip() for field in raw.split("\0") if field.strip()]
    tokens: set[str] = set()
    for index in range(0, len(fields) - 1, 2):
        name, email = fields[index], fields[index + 1]
        if len(name) >= 4:
            tokens.add(name.casefold())
        for word in re.findall(r"[A-Za-z][A-Za-z0-9_-]+", name):
            if len(word) >= 4:
                tokens.add(word.casefold())
        local = email.partition("@")[0]
        if len(local) >= 4 and not local.isdigit():
            tokens.add(local.casefold())
    return tokens


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def check_identity(files: list[Path], errors: list[str]) -> None:
    tokens = author_identity_tokens()
    for path in files:
        folded_path = path.as_posix().casefold()
        if any(
            re.search(rf"(?<![\w]){re.escape(token)}(?![\w])", folded_path)
            for token in tokens
        ):
            errors.append(f"{path}: author identity in repository-relative path")
        text = read_text(path)
        if text is None:
            continue
        folded = text.casefold()
        for token in sorted(tokens, key=len, reverse=True):
            match = re.search(rf"(?<![\w]){re.escape(token)}(?![\w])", folded)
            if match:
                errors.append(
                    f"{path}:{line_number(text, match.start())}: author identity outside metadata"
                )
                break
        match = HOME_PATH.search(text)
        if match:
            errors.append(f"{path}:{line_number(text, match.start())}: absolute home path")


def check_commit_messages(commit_range: str | None, errors: list[str]) -> None:
    if not commit_range:
        return
    tokens = author_identity_tokens()
    records = git("log", "--format=%H%x00%B%x00", commit_range).split("\0")
    for index in range(0, len(records) - 1, 2):
        oid, body = records[index].strip(), records[index + 1]
        if not oid:
            continue
        folded = body.casefold()
        if any(re.search(rf"(?<![\w]){re.escape(token)}(?![\w])", folded) for token in tokens):
            errors.append(f"commit {oid[:12]}: author identity appears in message")
        if HOME_PATH.search(body):
            errors.append(f"commit {oid[:12]}: absolute home path appears in message")
        subject = body.splitlines()[0] if body.splitlines() else ""
        if not COMMIT_SUBJECT.match(subject):
            errors.append(f"commit {oid[:12]}: subject must use '<scope>: <lowercase outcome>'")
        if NON_IMPERATIVE_START.match(subject):
            errors.append(f"commit {oid[:12]}: subject outcome must use imperative mood")
        if len(subject) > 72:
            errors.append(f"commit {oid[:12]}: subject exceeds 72 characters")
        if re.match(r"(?i)^(?:wip|fixup!|squash!)", subject):
            errors.append(f"commit {oid[:12]}: unpublished-work marker in subject")
        if any(pattern.search(body) for pattern in PROCESS_PATTERNS):
            errors.append(f"commit {oid[:12]}: implementation-history narration in message")


def check_markdown(files: list[Path], errors: list[str]) -> None:
    markdown = [path for path in files if path.suffix.lower() == ".md"]
    for path in markdown:
        text = read_text(path) or ""
        for match in MARKDOWN_LINK.finditer(text):
            target = match.group(1).strip().split(maxsplit=1)[0].strip("<>")
            if target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target = unquote(target.split("#", 1)[0])
            resolved = (ROOT / path.parent / target).resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                errors.append(
                    f"{path}:{line_number(text, match.start())}: link escapes repository: {target}"
                )
                continue
            if not resolved.exists():
                errors.append(
                    f"{path}:{line_number(text, match.start())}: broken local link: {target}"
                )

        if path == Path("docs/roadmap.md") or (
            len(path.parts) >= 3 and path.parts[0] == "docs" and path.parts[1] in STATUS_DIRS
        ):
            status = STATUS_FIELD.search("\n".join(text.splitlines()[:30]))
            if not status:
                errors.append(f"{path}: missing status field in first 30 lines")
            else:
                value = status.group(1).strip().casefold()
                valid = any(
                    value == prefix or value.startswith((prefix + " ", prefix + "("))
                    for prefix in STATUS_PREFIXES
                ) or value.startswith("superseded by ")
                if not valid:
                    errors.append(f"{path}: unsupported document status: {status.group(1).strip()}")

    active: list[Path] = []
    for path in markdown:
        if path.parts[:2] != ("docs", "plans"):
            continue
        status = STATUS_FIELD.search("\n".join((read_text(path) or "").splitlines()[:30]))
        if status and status.group(1).strip().casefold().startswith("in progress"):
            active.append(path)
    if len(active) > 1:
        errors.append(f"docs/plans: expected at most one In progress plan, found {len(active)}")


def check_line_budgets(files: list[Path], errors: list[str]) -> None:
    for path in files:
        text = read_text(path)
        if text is None:
            continue
        name = path.as_posix()
        for prefix, budget in LINE_BUDGETS.items():
            if name == prefix or (prefix.endswith("/") and name.startswith(prefix)):
                lines = len(text.splitlines())
                if lines > budget:
                    errors.append(f"{path}: {lines} lines exceeds {budget}-line budget")
                break


def check_process_narration(files: list[Path], errors: list[str]) -> None:
    for path in files:
        name = path.as_posix()
        if not (name == "xmake.lua" or name.startswith(PROCESS_ROOTS)):
            continue
        text = read_text(path)
        if text is None:
            continue
        reported_lines: set[int] = set()
        for pattern in PROCESS_PATTERNS:
            for match in pattern.finditer(text):
                line = line_number(text, match.start())
                if line in reported_lines:
                    continue
                reported_lines.add(line)
                errors.append(
                    f"{path}:{line}: implementation-history narration"
                )


def check_public_copy(files: list[Path], errors: list[str]) -> None:
    for path in files:
        if path not in PUBLIC_COPY:
            continue
        text = read_text(path)
        if text is None:
            continue
        for pattern, description in (
            (PUBLIC_PLANNING_LANGUAGE, "internal milestone language"),
            (PUBLIC_UNSHIPPED_BACKEND, "unimplemented backend"),
        ):
            match = pattern.search(text)
            if match:
                errors.append(f"{path}:{line_number(text, match.start())}: {description} in public copy")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    # No range means no commit is checked, so history imported from elsewhere is never judged.
    parser.add_argument(
        "--commits",
        help="git revision or range whose message text is checked; author fields are ignored",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors: list[str] = []
    try:
        files = repository_files()
        if not files:
            errors.append("no repository files found")
        check_identity(files, errors)
        check_commit_messages(args.commits, errors)
        check_markdown(files, errors)
        check_line_budgets(files, errors)
        check_process_narration(files, errors)
        check_public_copy(files, errors)
    except (OSError, RuntimeError) as exc:
        print(f"policy check could not run: {exc}", file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"project policy failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"project policy passed ({len(files)} repository files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
