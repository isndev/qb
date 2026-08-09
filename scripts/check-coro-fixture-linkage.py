#!/usr/bin/env python3
"""check-coro-fixture-linkage.py — a test fixture handed to a coroutine-spawning
framework template must have LINKAGE. Anonymous namespace is not it.

WHY THIS EXISTS
---------------
`qbm::http::ws::coro_session<Self, Server>` spawns its receive loop by handing a lambda to
`qb::io::async::CoroutineScheduler::spawn()`. The scheduler moves that callable into the
frame of `invoke_owned_<F>` (qb/src/qb/io/async/coroutine/scheduler.h), so a class DEFINED
IN A QB HEADER ends up with a field whose type is the closure. When `Self`/`Server` are
declared in the test's anonymous namespace, the closure type has NO LINKAGE, and gcc says so:

    qb/src/qb/io/async/coroutine/scheduler.h:793: error:
      'qb::io::async::CoroutineScheduler::invoke_owned_<qb::http::ws::coro_session<
       {anonymous}::BoundedSession, {anonymous}::BoundedServer>::spawn_run_loop()::<lambda()> >
       ...Frame' has a field ... whose type has no linkage [-Werror=subobject-linkage]

GCC's own advice for that diagnostic — "give the enclosing class internal linkage too" —
is unavailable: the enclosing class is in qb, and qb is not going to be per-test-file. The
fixture is the end that can move, so it moves: a named `<file>_test` namespace, which is the
convention `ws-lifecycle.cpp`, `ws-robustness.cpp` and `ws-client-echo.cpp` already used.

WHY A SCRIPT AND NOT JUST THE COMPILER
--------------------------------------
Because the compiler is a demonstrably unreliable oracle for this class. Measured at 3.0.0
with g++-14 14.2.0, `-O3`, `QB_TESTS_WERROR=ON`, over a full build of all 349 test targets:
FIVE test files carried the shape, across TWO repos, and gcc-14 diagnoses exactly ONE of
them (`ws-framing-edge.cpp`, twice — once per fixture pair). Exactly four qbm-http test
sources instantiate `coro_session` at all, and pre-fix all four passed anonymous-namespace
fixtures to it: `ws-framing-edge.cpp` plus `ws-coro-server.cpp`, `ws-coro-handoff.cpp` and
`ws-coro-handshake-negative.cpp`, which carry the identical shape and compile silently. The
fifth is qb's own `tests/io/unit/coroutine/stream-transforms.cpp`, which reaches the same
frame through a different carrier — so the set is NOT one repo's, and a check scoped to
qbm-http would miss a fifth of it. A rule enforced only by the instance gcc happens to
notice is not enforced.

(`ws-lifecycle.cpp`, `ws-robustness.cpp` and `ws-client-echo.cpp` are where the
`<file>_test` naming convention comes from, but they instantiate no carrier and were never
part of this set.)

The gcc-14 axis in `.github/workflows/qbm-tests.yml` still runs, and is still the thing that
catches an unrelated diagnostic. This script is the part that does not depend on which
instantiation gcc's front end decided to complain about.

WHAT IT CHECKS
--------------
For every `*.cpp` under each scanned root:

  1. Find every anonymous-namespace region (`namespace {` … matching `}`) and collect the
     class/struct names DECLARED inside it (including forward declarations, which is how
     the CRTP server type is always introduced).
  2. Find every use of a CARRIER template — a framework class template that spawns a
     coroutine — and split its explicit template-argument list at top level.
  3. Report any argument naming one of those anonymous-namespace types.

The carriers are DERIVED from the framework source, never typed here: `--framework <dir>`
roots are scanned for `template <…> class|struct NAME` whose brace-matched body contains a
`spawn(` call taking a lambda literal. At 3.0.0, with `--framework qb/src --framework
qbm/http/src`, that finds THREE — `async_stream` (qb/src/qb/io/async/coroutine/stream.h),
`batcher` (qb/src/qb/core/patterns/aggregate.h) and `coro_session`
(qbm/http/src/qbm/http/ws/coro.h) — the same three AGENTS.md names, and two of them are
qb's own, which is why the qb tree is scanned and not only the module that owns the
diagnosed file. The run PRINTS what it found — a carrier set that silently empties is the
vacuity this guard would otherwise die of, so an empty set is a hard failure.

WHAT IT DELIBERATELY DOES NOT CATCH
-----------------------------------
Deduced template arguments. `qb::ask_quorum<E>` (qb/src/qb/core/patterns/scatter.h) spawns
too, and an anonymous-namespace event type reaches its frame the same way — but nobody
writes `ask_quorum<MyEvent>`, they write `ask_quorum(ctx, targets, k, req, timeout)` and let
E deduce. There is no token for a textual check to see. CRTP bases like `coro_session` are
the shape that IS written out, and they are the shape this catches. Saying so here is the
point: the limit is stated, not hidden behind a green run.

ANTI-VACUOUS FLOORS
-------------------
Each root carries a `NAME:MIN` floor — the minimum number of `*.cpp` that must be VISITED —
and the floors are PER ROOT, never shared, so one tree collapsing cannot be absorbed by
another growing. Raise a floor when a tree grows; never lower one to make a run pass.
`--min-anon` floors the number of anonymous-namespace regions actually parsed, so a broken
region scanner reports as itself rather than as "no findings".

USAGE
-----
    ./scripts/check-coro-fixture-linkage.py                       # qb alone, from the qb root
    ./scripts/check-coro-fixture-linkage.py --framework qb/src --framework qbm/http/src \
        qb/tests:219 qbm/http/tests:117 qbm/pgsql/tests:37 qbm/redis/tests:44

Exit status: 0 clean, 1 findings, 2 usage/IO error.
"""

from __future__ import annotations

import os
import re
import sys

SKIP_DIRS = {".git", "build", "__pycache__", ".cache", "node_modules", ".venv", "vendor"}

FRAMEWORK_SUFFIXES = (".h", ".hpp")
TEST_SUFFIXES = (".cpp",)

# `template <...>` followed (possibly across the parameter list) by `class|struct NAME`.
CLASS_TEMPLATE_RE = re.compile(r"\btemplate\s*<", re.MULTILINE)
CLASS_HEAD_RE = re.compile(r"\b(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::|\{)")

# A declaration of a class/struct: `class X;`, `struct X {`, `class X : public …`.
DECL_RE = re.compile(r"\b(?:class|struct)\s+([A-Za-z_]\w*)\s*(?:;|:|\{)")

# `spawn(` immediately followed by a lambda literal.
SPAWN_LAMBDA_RE = re.compile(r"\bspawn\s*\(\s*\[")


def strip_comments(src: str) -> str:
    """Blank out // and /* */ comments, preserving offsets and line count."""
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
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote:
                    j += 1
                    break
                j += 1
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_brace(src: str, open_idx: int) -> int:
    """Index just past the `}` matching the `{` at open_idx, or len(src)."""
    depth = 0
    i, n = open_idx, len(src)
    while i < n:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def match_angle(src: str, open_idx: int) -> int:
    """Index just past the `>` matching the `<` at open_idx, or -1. Skips (), [], {}."""
    depth = 0
    i, n = open_idx, len(src)
    while i < n:
        c = src[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return i + 1
        elif c in "([{":
            j = i
            close = {"(": ")", "[": "]", "{": "}"}[c]
            d2 = 0
            while j < n:
                if src[j] == c:
                    d2 += 1
                elif src[j] == close:
                    d2 -= 1
                    if d2 == 0:
                        break
                j += 1
            i = j
        elif c == ";":
            return -1  # a `<` that was a comparison, not a template
        i += 1
    return -1


def split_top_level(args: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in args:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def walk(root: str, suffixes: tuple[str, ...]):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            if fn.endswith(suffixes):
                yield os.path.join(dirpath, fn)


def find_carriers(framework_roots: list[str]) -> dict[str, str]:
    """Class templates whose body spawns a lambda. name -> where it was found."""
    carriers: dict[str, str] = {}
    for root in framework_roots:
        for path in walk(root, FRAMEWORK_SUFFIXES):
            try:
                src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            except OSError:
                continue
            if not SPAWN_LAMBDA_RE.search(src):
                continue
            for m in CLASS_TEMPLATE_RE.finditer(src):
                end_params = match_angle(src, m.end() - 1)
                if end_params < 0:
                    continue
                head = CLASS_HEAD_RE.match(src, end_params) or CLASS_HEAD_RE.search(
                    src, end_params, end_params + 200
                )
                if not head:
                    continue
                body = src.find("{", head.end() - 1)
                if body < 0:
                    continue
                close = match_brace(src, body)
                if SPAWN_LAMBDA_RE.search(src, body, close):
                    line = src.count("\n", 0, head.start()) + 1
                    carriers.setdefault(head.group(1), f"{path}:{line}")
    return carriers


def anon_regions(src: str) -> list[tuple[int, int]]:
    """(start, end) byte spans of every `namespace {` … `}` region."""
    spans = []
    for m in re.finditer(r"\bnamespace\s*\{", src):
        open_idx = src.index("{", m.start())
        spans.append((open_idx, match_brace(src, open_idx)))
    return spans


def check_file(path: str, carriers: dict[str, str]) -> tuple[list[str], int]:
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ([f"{path}: unreadable"], 0)
    src = strip_comments(raw)
    spans = anon_regions(src)
    if not spans:
        return ([], 0)

    anon_types: dict[str, int] = {}
    for start, end in spans:
        for m in DECL_RE.finditer(src, start, end):
            anon_types.setdefault(m.group(1), src.count("\n", 0, m.start()) + 1)
    if not anon_types:
        return ([], len(spans))

    findings = []
    for carrier in sorted(carriers):
        for m in re.finditer(r"\b" + re.escape(carrier) + r"\s*<", src):
            close = match_angle(src, src.index("<", m.end() - 1))
            if close < 0:
                continue
            args = src[src.index("<", m.end() - 1) + 1 : close - 1]
            for arg in split_top_level(args):
                name = re.sub(r".*::", "", arg).strip().rstrip("*&").strip()
                if name in anon_types:
                    line = src.count("\n", 0, m.start()) + 1
                    findings.append(
                        f"{path}:{line}: {carrier}<… {name} …> — {name} is declared in an "
                        f"anonymous namespace (line {anon_types[name]}). {carrier} spawns a "
                        f"coroutine whose frame is defined in a qb header, so the frame gets a "
                        f"field of no-linkage type (-Wsubobject-linkage). Move the fixtures to a "
                        f"named `<file>_test` namespace."
                    )
    return (findings, len(spans))


def default_args() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return ["--framework", os.path.join(qb_root, "src"), f"{os.path.join(qb_root, 'tests')}:219"]


def main() -> int:
    argv = sys.argv[1:] or default_args()
    framework_roots: list[str] = []
    specs: list[str] = []
    min_anon = 1
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--framework":
            i += 1
            if i >= len(argv):
                print("usage error: --framework needs a directory", file=sys.stderr)
                return 2
            framework_roots.append(argv[i])
        elif a.startswith("--min-anon="):
            min_anon = int(a.split("=", 1)[1])
        elif a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        else:
            specs.append(a)
        i += 1

    if not framework_roots:
        print("usage error: at least one --framework <dir> is required", file=sys.stderr)
        return 2
    for r in framework_roots + [s.rsplit(":", 1)[0] for s in specs]:
        if not os.path.isdir(r):
            print(f"::error::no such root: {r}", file=sys.stderr)
            return 2

    carriers = find_carriers(framework_roots)
    if not carriers:
        print(
            "::error::no coroutine-spawning class template found in "
            f"{', '.join(framework_roots)} — this guard would pass vacuously. Either the "
            "framework changed shape or the scan broke; refusing to report a green.",
            file=sys.stderr,
        )
        return 2
    print("carriers (class templates that spawn a lambda into a coroutine frame):")
    for name in sorted(carriers):
        print(f"  {name}  <- {carriers[name]}")

    findings: list[str] = []
    total_visited = 0
    total_anon = 0
    for spec in specs:
        if ":" not in spec:
            print(f"usage error: root spec '{spec}' must be NAME:MIN", file=sys.stderr)
            return 2
        root, floor_s = spec.rsplit(":", 1)
        try:
            floor = int(floor_s)
        except ValueError:
            print(f"usage error: root spec '{spec}' must be NAME:MIN", file=sys.stderr)
            return 2

        visited = 0
        anon = 0
        for path in walk(root, TEST_SUFFIXES):
            visited += 1
            f, a = check_file(path, carriers)
            findings.extend(f)
            anon += a
        print(f"  {root}: {visited} test sources visited (floor {floor}), {anon} anonymous-namespace regions")
        if visited < floor:
            print(f"::error::{root} visited {visited} sources, below its floor of {floor}", file=sys.stderr)
            return 2
        total_visited += visited
        total_anon += anon

    if total_anon < min_anon:
        print(
            f"::error::only {total_anon} anonymous-namespace regions parsed across "
            f"{total_visited} sources (floor {min_anon}); the region scanner is not working, "
            "so a clean result would be vacuous.",
            file=sys.stderr,
        )
        return 2

    if findings:
        for f in findings:
            print(f"::error::{f}", file=sys.stderr)
        print(f"\n{len(findings)} finding(s).", file=sys.stderr)
        return 1

    print(f"OK: {total_visited} test sources, {total_anon} anonymous-namespace regions, 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
