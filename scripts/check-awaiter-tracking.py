#!/usr/bin/env python3
"""check-awaiter-tracking.py — every awaiter of the framework says what its coroutine waits on.

WHY THIS EXISTS
---------------
`CoroutineScheduler::dump()` (Huly QB-71) reports, per parked coroutine, what it waits on and
for how long. The record is written by the AWAITER: the first statement of its `await_suspend`
is a `track_suspension(...)` call — `detail::track_suspension(h.address(), kind)` in qb, the
public `qb::io::async::track_suspension(h, "kind")` in the modules — one predictable branch when
tracking is off. Nothing else writes it, and a record is replaced only by the coroutine's NEXT
recorded suspension. So an awaiter that forgets the call does not merely go unlabelled: the
coroutine parked on it keeps the record of its PREVIOUS wait, and the dump shows it waiting on
something it left long ago, for an age that keeps growing. Nothing fails — not the compiler, not
a test that does not dump that exact awaiter. Measured when the dump landed: qb's own 49
awaiters were instrumented by the change, and the five of qbm-http, qbm-pgsql and qbm-redis —
an HTTP request, a query, a command, two connects — were found missing in review, which is the
shape the next awaiter would take.

WHAT IT CHECKS
--------------
For every `.h`/`.cpp` under each subject root, every DEFINITION of a member `await_suspend(...)`
(a body follows the parameter list and its specifiers; a call `x.await_suspend(h)`, a declaration
`... = 0;` or `...;` is not one):

  A1  its first statement must be a `track_suspension(` call (any qualification) -- or, for an awaiter
      that TRANSFERS to another coroutine, the gated tail record
      `if (tracking_on()) [[unlikely]] return track_then(...);` with no `return` before it and
      no `resume(` anywhere in the body. That second shape records after the awaiter's own work, so it
      is only sound where nothing resumes inline; it exists for MSVC, which does not shrink-wrap: a call
      anywhere puts its stack frame on the fast path, and `track_then` makes none -- it hands the
      suspension to this thread's recorder coroutine (`task<T>::await_suspend`: 6 instructions executed
      without the hook, 21 with a call first, 15 with it last, 10 with the recorder -- /FAs).

EXEMPT, STRUCTURALLY: an awaiter defined inside a `final_suspend()` body. Its coroutine is
finishing, not parked — the frame's destruction erases the record (the promise destructors of
`task` and `async_generator`).

ESCAPE HATCH: `// awaiter-tracking: <reason>` on the definition's line or one of the two lines
above it. The reason is MANDATORY: an awaiter that never parks, or an INNER awaiter whose outer
awaiter records first and then delegates its `await_suspend` to it, must say which. One today:
`qb::detail::ask_awaiter`, driven by `qb::ask`'s two awaiters in core/patterns/request.h — a
second record there would be the same frame, the same kind, and one more branch per ask.

WHAT IT DELIBERATELY DOES NOT CATCH
-----------------------------------
It is textual. An `await_suspend` produced by a macro, or whose first statement is a call that
records indirectly, is invisible to it or needs the annotation. It does not judge the KIND label.
An awaiter in an application's own code is the application's business; the public
`track_suspension(h, kind)` is documented for it.

ANTI-VACUOUS FLOORS
-------------------
Each subject root is `NAME:MIN_FILES[:MIN_AWAITERS]` — the files VISITED and the `await_suspend`
definitions CHECKED, per root, never shared: one tree collapsing cannot hide behind another
growing, and a broken definition scanner reports as itself rather than as "no findings".

USAGE
-----
    ./scripts/check-awaiter-tracking.py                       # qb alone, from the qb root
    ./scripts/check-awaiter-tracking.py qb/src:160:49 qbm/http/src:105:1 qbm/pgsql/src:37:2 \
        qbm/redis/src:29:2

Exit status: 0 clean, 1 findings, 2 usage/IO error or a vacuous run.
"""

from __future__ import annotations

import os
import re
import sys

SKIP_DIRS = {".git", "build", "__pycache__", ".cache", "node_modules", ".venv", "vendor"}
SUFFIXES = (".h", ".cpp")

AWAIT_SUSPEND_RE = re.compile(r"\bawait_suspend\s*\(")
TRACK_RE = re.compile(r"\s*(?:::)?(?:[A-Za-z_]\w*\s*::\s*)*track_suspension\s*\(")
# the transfer shape's gated tail record, and what would make it unsound
TAIL_RE = re.compile(
    r"\bif\s*\(\s*(?:::)?(?:[A-Za-z_]\w*\s*::\s*)*tracking_on\s*\(\s*\)\s*\)\s*(?:\[\[\s*unlikely\s*\]\]\s*)?"
    r"return\s+(?:::)?(?:[A-Za-z_]\w*\s*::\s*)*track_then\s*\("
)
RETURN_RE = re.compile(r"\breturn\b")
RESUME_RE = re.compile(r"(?:\.|->)\s*resume\s*\(")
HEAD_NAME_RE = re.compile(r"\b(?:class|struct)\s+(?:alignas\s*\([^)]*\)\s*)?([A-Za-z_]\w*)")
ANNOTATION_RE = re.compile(r"awaiter-tracking:\s*(\S.*)")
SPECIFIER_RE = re.compile(r"\s*(?:const\b|noexcept\b(?:\s*\([^()]*\))?|override\b|final\b|&&|&)")


def strip_comments(src: str) -> str:
    """Blank // and /* */ comments, string/char literals and preprocessor lines, preserving offsets."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c == "R" and nxt == '"':
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i)
            end = n if end == -1 else end + len(m.group(1)) + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:end]))
            i = end
        elif c in "\"'":
            if c == "'" and i > 0 and (src[i - 1].isalnum() or src[i - 1] == "_"):
                out.append(c)  # digit separator: 1'000
                i += 1
                continue
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    text = "".join(out)
    # a preprocessor line (and its continuations) is not a statement
    lines = text.split("\n")
    k = 0
    while k < len(lines):
        if lines[k].lstrip().startswith("#"):
            while k < len(lines):
                cont = lines[k].rstrip().endswith("\\")
                lines[k] = " " * len(lines[k])
                k += 1
                if not cont:
                    break
            continue
        k += 1
    return "\n".join(lines)


def match_paren(src: str, open_idx: int) -> int:
    """Index just past the `)` matching the `(` at open_idx, or -1."""
    depth, i, n = 0, open_idx, len(src)
    while i < n:
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def brace_spans(src: str) -> list[tuple[int, int]]:
    """(open, close) of every brace pair."""
    spans, stack = [], []
    for i, c in enumerate(src):
        if c == "{":
            stack.append(i)
        elif c == "}" and stack:
            spans.append((stack.pop(), i))
    return spans


def head_of(src: str, open_idx: int) -> str:
    """The text that introduces the brace at open_idx: back to the previous `;`, `{` or `}`."""
    j = open_idx - 1
    while j >= 0 and src[j] not in ";{}":
        j -= 1
    return src[j + 1 : open_idx]


def walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            if fn.endswith(SUFFIXES):
                yield os.path.join(dirpath, fn)


def scan_file(path: str) -> tuple[list[str], int, int, int, int]:
    """(findings, awaiters checked, awaiters exempt as final awaiters, awaiters recording by the transfer shape,
    awaiters annotated -- which record nothing themselves)."""
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ([f"{path}: unreadable"], 0, 0, 0, 0)
    if "await_suspend" not in raw:
        return ([], 0, 0, 0, 0)
    src = strip_comments(raw)
    raw_lines = raw.split("\n")
    spans = None
    findings: list[str] = []
    checked = exempt = transfer = annotated = 0
    for m in AWAIT_SUSPEND_RE.finditer(src):
        before = src[: m.start()].rstrip()
        if before.endswith((".", "->")):
            continue  # a call through an object
        close = match_paren(src, m.end() - 1)
        if close < 0:
            continue
        k = close
        while True:  # const, noexcept(...), override, final, ref-qualifiers
            sm = SPECIFIER_RE.match(src, k)
            if not sm or sm.end() == k:
                break
            k = sm.end()
        rest = src[k:].lstrip()
        if rest.startswith("->"):  # trailing return type
            brace = src.find("{", k)
            semi = src.find(";", k)
            if brace < 0 or (0 <= semi < brace):
                continue
            k = brace
            rest = src[k:]
        if not rest.startswith("{"):
            continue  # a declaration (`;`, `= 0;`) or a call in an expression
        body = src.index("{", k)
        if spans is None:
            spans = brace_spans(src)
        enclosing = [(o, c) for o, c in spans if o < m.start() < c]
        if any(re.search(r"\bfinal_suspend\s*\(", head_of(src, o)) for o, _ in enclosing):
            exempt += 1
            continue
        checked += 1
        line = src.count("\n", 0, m.start()) + 1
        note = " ".join(raw_lines[max(0, line - 3) : line])
        ann = ANNOTATION_RE.search(note)
        if ann and ann.group(1).strip():
            annotated += 1
            continue
        if TRACK_RE.match(src, body + 1):
            continue
        # the transfer shape: the gated tail record, before any return, in a body that resumes nothing inline
        body_end = next((c for o, c in spans if o == body), len(src))
        text = src[body + 1 : body_end]
        tail = TAIL_RE.search(text)
        transfer_note = ""
        if tail:
            if RETURN_RE.search(text, 0, tail.start()):
                transfer_note = " -- its gated tail record comes after a `return`, so a path leaves unrecorded"
            elif RESUME_RE.search(text):
                transfer_note = (" -- its body calls resume(), and the tail record is only sound where nothing resumes inline "
                                 "(it records after the awaiter's work)")
            else:
                transfer += 1
                continue
        cls = "?"
        qualified = re.search(r"([A-Za-z_]\w*)\s*(?:<[^<>;{}]*>)?\s*::\s*$", before)
        if qualified:  # an out-of-class definition: `Name::await_suspend(...) { ... }`
            cls = qualified.group(1)
        else:
            for o, _ in sorted(enclosing, key=lambda s: s[0], reverse=True):
                hm = HEAD_NAME_RE.search(head_of(src, o))
                if hm:
                    cls = hm.group(1)
                    break
        first = src[body + 1 : src.find(";", body + 1) + 1].strip().replace("\n", " ")
        findings.append(
            f"{path}:{line}: [A1] {cls}::await_suspend does not begin with a track_suspension(...) call "
            f"(its first statement is `{re.sub(r' +', ' ', first)[:80]}`){transfer_note}, so a coroutine parked on "
            f"it keeps the record of its PREVIOUS wait in CoroutineScheduler::dump(). Begin the body with "
            f"`::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);` (qb) or "
            f"`qb::io::async::track_suspension(h, \"<kind>\");` (a module); an awaiter that transfers to another "
            f"coroutine may instead end with `if (tracking_on()) [[unlikely]] return track_then(...);` "
            f"before any return; or annotate `// awaiter-tracking: <reason>` if it never parks."
        )
    return findings, checked, exempt, transfer, annotated


def default_args() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Measured at 3.3.0 on qb alone (Huly QB-71): 188 files, 49 awaiters checked, 3 final awaiters exempt.
    return [f"{os.path.join(qb_root, 'src')}:160:49"]


def main() -> int:
    argv = sys.argv[1:] or default_args()
    specs: list[tuple[str, int, int]] = []
    for a in argv:
        if a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        parts = a.rsplit(":", 2)
        try:
            if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
                specs.append((parts[0], int(parts[1]), int(parts[2])))
            else:
                root, floor = a.rsplit(":", 1)
                specs.append((root, int(floor), 0))
        except ValueError:
            print(f"usage error: root spec '{a}' must be NAME:MIN_FILES[:MIN_AWAITERS]", file=sys.stderr)
            return 2
    for root, _, _ in specs:
        if not os.path.isdir(root):
            print(f"::error::no such root: {root}", file=sys.stderr)
            return 2

    findings: list[str] = []
    total_files = total_checked = total_exempt = total_transfer = total_annotated = 0
    for root, min_files, min_awaiters in specs:
        visited = checked = exempt = transfer = annotated = 0
        for path in walk(root):
            visited += 1
            f, c, e, x, a = scan_file(path)
            findings.extend(f)
            checked += c
            exempt += e
            transfer += x
            annotated += a
        print(f"  {root}: {visited} sources+headers visited (floor {min_files}), {checked} await_suspend definitions "
              f"checked (floor {min_awaiters}), {exempt} final awaiter(s) exempt")
        if visited < min_files:
            print(f"::error::{root} visited {visited} sources, below its floor of {min_files}", file=sys.stderr)
            return 2
        if checked < min_awaiters:
            print(f"::error::{root}: {checked} await_suspend definitions checked, below its floor of {min_awaiters}; "
                  "the definition scanner is not working or the tree lost its awaiters -- refusing a vacuous green.",
                  file=sys.stderr)
            return 2
        total_files += visited
        total_checked += checked
        total_exempt += exempt
        total_transfer += transfer
        total_annotated += annotated

    if findings:
        for f in findings:
            print(f"::error::{f}", file=sys.stderr)
        print(f"\n{len(findings)} finding(s).", file=sys.stderr)
        return 1

    first = total_checked - total_transfer - total_annotated
    print(f"OK: {total_files} sources+headers, {total_checked} await_suspend definitions checked ({first} record first, "
          f"{total_transfer} by the transfer shape, {total_annotated} annotated), {total_exempt} final awaiters exempt, "
          f"0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
