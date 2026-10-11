#!/usr/bin/env python3
"""PreToolUse hook: keep prose out of source files.

Claude Code runs this before Write / Edit / NotebookEdit. Exit 0 allows the
call; exit 2 blocks it and feeds stderr back to Claude as the reason.

Modes, via the CLAUDE_COMMENT_POLICY environment variable:

    redundant   block only comments that restate the code  (default)
    strict      block every comment except the allowlist
    off         allow everything

The default is deliberate. Comments that restate the line below them are pure
noise and worth blocking. Comments that record why a non-obvious choice was
made are the cheapest documentation there is, and they are read at exactly the
moment the question arises. Blocking both costs more than it saves.

Design note: explanations are redirected to a Markdown file keyed by SYMBOL,
never by line number. Line numbers in a sidecar document are wrong the first
time anyone edits the file above them.
"""

import json
import os
import re
import sys

NOTES_FILE = os.environ.get("CLAUDE_COMMENT_NOTES", "docs/code-notes.md")
MODE = os.environ.get("CLAUDE_COMMENT_POLICY", "redundant").lower()

C_FAMILY = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp",
            ".cs", ".java", ".js", ".jsx", ".ts", ".tsx", ".go", ".rs",
            ".swift", ".kt", ".m", ".mm", ".php", ".scala", ".glsl", ".hlsl"}
HASH_FAMILY = {".py", ".sh", ".bash", ".zsh", ".rb", ".pl", ".r",
               ".yml", ".yaml", ".toml", ".cmake", ".mk", ".tf"}
HASH_NAMES = {"CMakeLists.txt", "Makefile", "Dockerfile"}

# Comments that are machine-readable instructions, not prose. Always allowed.
ALLOW = [
    re.compile(r"^\s*(//|#)\s*SPDX-License-Identifier:"),
    re.compile(r"^\s*//\s*NOLINT"),
    re.compile(r"^\s*//\s*clang-format\s+(on|off)"),
    re.compile(r"^\s*//\s*IWYU pragma:"),
    re.compile(r"^\s*#!\s*/"),
    re.compile(r"^\s*#\s*-\*-\s*coding"),
    re.compile(r"^\s*#\s*(noqa|type:|pylint:|mypy:|ruff:|fmt:)"),
    re.compile(r"^\s*#\s*(!\[|\[)"),
]

# The same directives, matched against the comment BODY, so they are still
# allowed when they trail real code (`int y = 0;  // NOLINT`).
ALLOW_BODY = [
    re.compile(r"^SPDX-License-Identifier:"),
    re.compile(r"^NOLINT"),
    re.compile(r"^clang-format\s+(on|off)"),
    re.compile(r"^IWYU pragma:"),
    re.compile(r"^(noqa|type:|pylint:|mypy:|ruff:|fmt:)"),
]

# Heuristics for "this comment restates the line under it".
RESTATES = [
    re.compile(r"^(get|set|return|increment|decrement|loop over|iterate|call|"
               r"check|create|delete|add|remove|initialise|initialize|"
               r"constructor|destructor|getter|setter)\b", re.I),
    re.compile(r"^(the )?(above|below|following) (code|line|function|block)", re.I),
]

# ---------------------------------------------------------------------------


def strip_literals(line, c_family):
    """Blank out string/char literal contents so a // inside one is not a comment."""
    out, i, n = [], 0, len(line)
    while i < n:
        ch = line[i]
        if ch in ("'", '"'):
            quote = ch
            out.append(" ")
            i += 1
            while i < n:
                if line[i] == "\\" and c_family:
                    i += 2
                    continue
                if line[i] == quote:
                    break
                i += 1
            out.append(" ")
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def comment_lines(text, path):
    """Yield (lineno, raw_line, comment_body) for each comment found."""
    _, ext = os.path.splitext(path)
    base = os.path.basename(path)
    c_family = ext.lower() in C_FAMILY
    hash_family = ext.lower() in HASH_FAMILY or base in HASH_NAMES
    if not (c_family or hash_family):
        return

    in_block = False
    for no, raw in enumerate(text.splitlines(), 1):
        if any(p.search(raw) for p in ALLOW):
            continue

        if in_block:
            body = raw
            if "*/" in raw:
                in_block = False
                body = raw.split("*/", 1)[0]
            if body.strip(" \t*"):
                yield no, raw, body.strip(" \t*")
            continue

        scrubbed = strip_literals(raw, c_family)

        if c_family:
            if "/*" in scrubbed:
                after = raw.split("/*", 1)[1]
                if "*/" in after:
                    body = after.split("*/", 1)[0]
                else:
                    in_block = True
                    body = after
                if body.strip(" \t*"):
                    yield no, raw, body.strip(" \t*")
                continue
            idx = scrubbed.find("//")
            if idx >= 0:
                body = raw[idx + 2:].strip()
                if body:
                    yield no, raw, body
        elif hash_family:
            idx = scrubbed.find("#")
            if idx >= 0:
                body = raw[idx + 1:].strip()
                if body:
                    yield no, raw, body


def looks_redundant(body):
    return any(p.search(body) for p in RESTATES)


def main():
    if MODE == "off":
        return 0

    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0  # never break the session on a malformed payload

    tool = payload.get("tool_name", "")
    data = payload.get("tool_input", {}) or {}

    if tool == "Write":
        path, text = data.get("file_path", ""), data.get("content", "")
    elif tool == "Edit":
        path, text = data.get("file_path", ""), data.get("new_string", "")
    elif tool == "NotebookEdit":
        path, text = data.get("notebook_path", ""), data.get("new_source", "")
    else:
        return 0

    if not path or not text:
        return 0

    # Never police the notes file itself, or Markdown generally.
    if path.endswith(".md"):
        return 0

    found = [f for f in comment_lines(text, path)
             if not any(p.search(f[2]) for p in ALLOW_BODY)]
    if MODE == "redundant":
        found = [f for f in found if looks_redundant(f[2])]
    if not found:
        return 0

    shown = found[:8]
    lines = [
        "Comment policy: this comment restates the code rather than explaining it.",
        "",
        "Blocked {} comment(s) in {}:".format(len(found), path),
    ]
    for no, _raw, body in shown:
        snippet = body if len(body) <= 70 else body[:67] + "..."
        lines.append("  line {}: {}".format(no, snippet))
    if len(found) > len(shown):
        lines.append("  ... and {} more".format(len(found) - len(shown)))
    lines += [
        "",
        "Re-send this edit with the comments removed. If any of them explained",
        "something a reader genuinely needs, put it in {} under a".format(NOTES_FILE),
        "heading that names the SYMBOL, not a line number:",
        "",
        "    ## {} :: <function or identifier>".format(os.path.basename(path)),
        "",
        "    <the explanation>",
        "",
        "Line-number references rot on the next edit; symbol names survive.",
        "",
        "Allowed without change: SPDX headers, NOLINT, clang-format on/off,",
        "IWYU pragmas, shebangs, and linter directives.",
    ]
    sys.stderr.write("\n".join(lines) + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
