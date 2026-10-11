# Comment policy hook

A Claude Code `PreToolUse` hook that blocks comments in source files and
redirects the explanation to a Markdown file.

## Install

**Per project** — copy the `hooks` and `env` blocks from
`settings.snippet.json` into `.claude/settings.json` at the repo root. Commit
it and everyone working in that repo gets the policy.

**Globally, all projects** — put the same blocks in `~/.claude/settings.json`
and use an absolute path to the script instead of `$CLAUDE_PROJECT_DIR`.

Claude Code reads settings at session start, so restart the session after
installing. Verify with `/hooks`.

## Modes

Set `CLAUDE_COMMENT_POLICY`:

| Value | Behaviour |
| --- | --- |
| `redundant` | Block only comments that restate the code (`// getter for name`). This is the default. |
| `strict` | Block every comment except the allowlist. |
| `off` | Disable without uninstalling. |

`CLAUDE_COMMENT_NOTES` sets the notes file path, default `docs/code-notes.md`.

## Why `redundant` is the default

`strict` was the original request. It is one environment variable away if you
want it, but the default is `redundant` because the two kinds of comment have
very different value.

Restating comments are noise and this hook kills them. Comments that record a
non-obvious decision are the cheapest documentation available and are read at
the exact moment the question comes up. In this repo that includes: why `/W4
/WX` must not go in `CMAKE_CXX_FLAGS`, why `IsolateForRuntime` exists at all,
why a `callsiteTarget` is deliberately not enabled yet, and which Havok struct
each raw offset in `AnimGraphLayout` belongs to. Each of those was learned by
losing time to it. None can be recovered by reading the code.

Set `CLAUDE_COMMENT_POLICY=strict` to get the original behaviour.

## What it does

Inspects `Write` / `Edit` / `NotebookEdit` payloads. Exit 0 allows, exit 2
blocks and returns the reason to Claude, which then re-sends the edit without
comments and writes the explanation to the notes file.

Understands `//` and `/* */` for C-family files and `#` for hash-comment files
and `CMakeLists.txt` / `Makefile` / `Dockerfile`. Other extensions pass through
unchecked. `#include` and `#pragma` in C-family files are preprocessor
directives, not comments, and are not flagged. String literals are blanked
before scanning, so `"http://example.com"` is not a comment. Markdown files are
never policed.

Always allowed, inline or on their own line: `SPDX-License-Identifier`,
`NOLINT`, `clang-format on/off`, `IWYU pragma:`, shebangs, and linter
directives like `# noqa` and `# type:`.

## Notes are keyed by symbol, not line number

The block message tells Claude to write:

```markdown
## Hooks.cpp :: HookedRunActorUpdates

The two arming conditions are not mutually exclusive and the order matters...
```

Not `Hooks.cpp:372`. A line number in a sidecar file is wrong the first time
anyone inserts a line above it. `Addresses.h` in this repo shifted its line
numbers four times in a single afternoon. Symbol names survive edits; line
numbers do not. If you change nothing else here, keep this.

## Known limits

- Heuristics, not a parser. Raw string literals (`R"(...)"`), line
  continuations inside `//` comments, and `#` inside some shell constructs can
  be misjudged. It errs toward allowing.
- `redundant` mode matches a small pattern list. It will miss restating
  comments phrased unusually and can flag a legitimate comment that happens to
  start with "check" or "return".
- It sees only the text being written, not the file around it, so it cannot
  tell whether an explanation is already in the notes file.
- It governs Claude's edits. It does not stop a human typing a comment, and it
  does not remove comments already in the tree.

## Turning it off

Delete the `PreToolUse` block, or set `CLAUDE_COMMENT_POLICY=off`.
