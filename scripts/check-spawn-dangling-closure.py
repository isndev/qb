#!/usr/bin/env python3
"""Lint: forbid dangling coroutine lambda closures.

`scheduler.spawn([..]() -> task<void> { ... }())` is undefined behavior:
`task<void>::promise_type::initial_suspend()` is `suspend_always`, so the
coroutine body only starts on the next `run_ready()` — by which point the
temporary closure created inside the spawn(...) full-expression has been
destroyed. Every capture access then goes through a dangling closure pointer
into a reused stack slot, producing intermittent, ASan-invisible stack
corruption (see qb-io coroutine docs in coroutine/scheduler.h).

The same lifetime rule applies to any immediately-invoked task-returning lambda
with captures, e.g. `with_timeout([cap]() -> task<int> { ... }(), 100ms)`.
Store the lambda in a local variable that lives in the parent coroutine frame,
or pass state as value parameters to a non-lambda coroutine helper.

The safe spelling drops the trailing `()` so the `spawn(Callable)` overload
moves the closure into an owning wrapper coroutine frame:

    scheduler.spawn([..]() -> task<void> { ... });   // SAFE

    auto op = [cap]() -> task<int> { ... };
    co_await with_timeout(op(), 100ms);              // SAFE

Usage:
    python3 qb/scripts/check-spawn-dangling-closure.py [root ...]

Scans the given roots (default: repository checkout containing this script)
for *.cpp / *.h / *.hpp / *.tpp and exits non-zero listing every offending site.
Documentation blocks (lines reading as comments) are skipped via a simple
comment-stripping pass, so the BROKEN examples in doc comments don't trip it.
"""
import re
import sys
import glob
import os


def strip_comments(src: str) -> str:
    """Blank out // and /* */ comments, preserving offsets/newlines."""
    out = []
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
            # skip string/char literal
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
            out.append(src[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_sites(path: str):
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    if "spawn" not in raw:
        raw_has_spawn = False
    else:
        raw_has_spawn = True
    src = strip_comments(raw)
    sites = []

    if raw_has_spawn:
        for m in re.finditer(r"\bspawn\s*\(", src):
            start = m.end()
            if not src[start : start + 80].lstrip().startswith("["):
                continue
            depth, i = 1, start
            while i < len(src) and depth > 0:
                ch = src[i]
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                i += 1
            arg = src[start : i - 1]
            if re.search(r"\}\s*\(\s*\)\s*$", arg.rstrip()):
                sites.append((
                    src[: m.start()].count("\n") + 1,
                    "spawn(<immediately-invoked lambda>) — dangling closure "
                    "(UB). Drop the trailing '()' so the spawn(Callable) "
                    "overload owns the closure.",
                ))

    if "task<" in raw:
        for m in re.finditer(
            r"\[([^\]]+)\]\s*\([^)]*\)\s*(?:mutable\s*)?->\s*task\s*<[^>]+>\s*\{",
            src,
        ):
            depth, i = 1, m.end()
            while i < len(src) and depth > 0:
                ch = src[i]
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                i += 1
            if re.match(r"\s*\(\s*\)", src[i : i + 8]):
                sites.append((
                    src[: m.start()].count("\n") + 1,
                    "captured task lambda is immediately invoked — dangling "
                    "closure (UB). Store the lambda in a local variable or "
                    "move the state into coroutine parameters.",
                ))

    return sites


def main() -> int:
    roots = sys.argv[1:] or [
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ]
    failed = False
    for root in roots:
        files = []
        for ext in ("cpp", "h", "hpp", "tpp"):
            files += glob.glob(os.path.join(root, "**", f"*.{ext}"), recursive=True)
        for f in sorted(set(files)):
            if os.sep + "build" + os.sep in f or os.sep + "modules" + os.sep in f:
                continue
            for line, message in find_sites(f):
                failed = True
                print(f"{f}:{line}: error: {message}")
    if failed:
        return 1
    print("OK: no dangling coroutine lambda closures found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
