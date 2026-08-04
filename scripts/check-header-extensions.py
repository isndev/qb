#!/usr/bin/env python3
"""check-header-extensions.py — `.h` is the only header extension. Keep it that way.

WHY THIS EXISTS
---------------
Through 2.6.0 qb shipped four `.tpp` and qbm shipped one `.tpp` + three `.inl`. 3.0 retired
all eight: every definition moved into the header that completes it, and the extension
disappeared. Nothing dropped -- the counts match per file on both axes -- but the *reason*
the extension existed did not survive contact with measurement:

  * The `.tpp` were believed to prevent a multiple-definition defect. They did not. All 41
    definitions in qb's four files were templates; a TU forced to instantiate every body
    emitted 136 weak external, 150 local and exactly ONE strong external symbol -- the
    probe's own function. 802 commits contain zero `multiple definition` / `duplicate
    symbol` / `LNK2005` occurrences. Linkage is a property of the declaration, not of the
    file name (dev/analysis/TPP-INVESTIGATION.md §1, §3).
  * They cost real things: three of the eight could not compile alone, so three were named
    exclusions in scripts/check-installed-headers.sh -- installed public files permanently
    outside the self-containment gate. Merging emptied that category.

So the rule is now "one header extension", and this script is what makes it a rule instead
of a habit. `.cursor/rules/cpp.mdc` used to instruct the opposite ("Template definitions go
in `.tpp` files") -- a standing instruction to reintroduce exactly what 3.0 removed. That
line is gone, but an instruction is advice and a check is a gate.

WHAT IT FORBIDS
---------------
  1. Any file named `*.tpp` or `*.inl` anywhere under a scanned root.
  2. Any `#include` naming a `*.tpp` or `*.inl`. Redundant with (1) while (1) holds, and the
     point is that it stays true if someone adds the include first and the file second --
     or resurrects a file from a tag into a tree that no longer has it.

WHAT IT DELIBERATELY DOES *NOT* TOUCH
-------------------------------------
The `PATTERN "*.tpp"` / `PATTERN "*.inl"` lines in qb/cmake/qbPackage.cmake. Those are
install *nets*, not dependencies: a FILES_MATCHING pattern that matches no file costs
nothing, and the failure it prevents -- a fragment that exists in source, is not installed,
and breaks a consumer's first TU -- is silent at configure time AND at install time. This
script is what makes those patterns redundant; deleting them would make the silent failure
reachable again the moment this check is bypassed. Keep both.

`.hpp` is not forbidden. The five `.hpp` in the tree are vendored (ska_hash, nlohmann,
uuid/catch) or test-local (qbm/pgsql/tests/shared); none is qb-authored public surface, and
renaming a vendored file is how a vendor drop becomes a merge conflict forever.

ANTI-VACUOUS FLOOR
------------------
A guard that walks a moved path finds nothing and passes. Each root carries a `NAME:MIN`
floor -- the minimum number of files that must be *visited* -- and the floors are PER ROOT,
never shared: a shared floor lets one root silently absorb another's collapse. Raise a floor
when a tree grows; never lower one to make a run pass.

ALLOWLIST
---------
`ALLOWED` below is empty and is meant to stay empty. If a future vendored drop ships `.inl`,
record it there with a reason; entries are CHECKED (a stale one fails the run), so an
allowlist cannot outlive the file it names.

USAGE
-----
    ./scripts/check-header-extensions.py                     # qb alone, from the qb root
    ./scripts/check-header-extensions.py qb:600 qbm/http:250 …

Exit status: 0 clean, 1 findings, 2 usage/IO error.
"""

from __future__ import annotations

import os
import re
import sys

FORBIDDEN_SUFFIXES = (".tpp", ".inl")

# Directories never walked: build output, VCS metadata, editor/tool caches.
SKIP_DIRS = {".git", "build", "__pycache__", ".cache", "node_modules", ".venv"}

# Sources searched for a forbidden #include.
INCLUDE_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".ipp", ".c", ".cc", ".cpp", ".cxx")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

# path -> reason. CHECKED: every entry must still exist, or the run fails.
ALLOWED: dict[str, str] = {}


def default_roots() -> list[str]:
    """qb alone, floored, when invoked with no arguments (qb's own CI)."""
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return [f"{qb_root}:600"]


def walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            yield os.path.join(dirpath, fn)


def main() -> int:
    specs = sys.argv[1:] or default_roots()
    findings: list[str] = []
    total_visited = 0
    total_scanned = 0

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
        if not os.path.isdir(root):
            print(f"::error::no such root: {root}", file=sys.stderr)
            return 2

        visited = 0
        scanned = 0
        for path in walk(root):
            visited += 1
            rel = os.path.relpath(path, root)
            if path.endswith(FORBIDDEN_SUFFIXES):
                if rel in ALLOWED:
                    continue
                findings.append(
                    f"{path}:1: error: forbidden header extension "
                    f"'{os.path.splitext(path)[1]}' -- `.h` is the only header extension in "
                    f"qb/qbm since 3.0. Merge these definitions into the header that "
                    f"completes them (the one whose declarations they define), the way the "
                    f"eight retired fragments were."
                )
                continue
            if not path.endswith(INCLUDE_SUFFIXES):
                continue
            scanned += 1
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for n, line in enumerate(fh, 1):
                        m = INCLUDE_RE.match(line)
                        if m and m.group(1).endswith(FORBIDDEN_SUFFIXES):
                            findings.append(
                                f"{path}:{n}: error: #include of a forbidden header "
                                f"extension: {m.group(1)}"
                            )
            except OSError as exc:  # pragma: no cover - IO shape, not logic
                print(f"::error::cannot read {path}: {exc}", file=sys.stderr)
                return 2

        if visited < floor:
            print(
                f"::error::VACUOUS: {root} yielded {visited} files, floor is {floor}. "
                f"The path is wrong or the tree collapsed; a pass here proves nothing.",
                file=sys.stderr,
            )
            return 2
        total_visited += visited
        total_scanned += scanned

    # A stale allowlist entry is how an exemption outlives the file it names.
    stale = [p for p in ALLOWED if not any(os.path.exists(os.path.join(s.rsplit(":", 1)[0], p)) for s in specs)]
    if stale:
        print(f"::error::stale allowlist entr(ies), no such file: {' '.join(stale)}", file=sys.stderr)
        return 2

    if findings:
        for f in findings:
            print(f)
        print(f"\n{len(findings)} finding(s). See {os.path.basename(__file__)} for why `.h` is the only extension.")
        return 1

    print(
        f"OK — {total_visited} files visited across {len(specs)} root(s), "
        f"{total_scanned} searched for forbidden includes; 0 .tpp / .inl."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
