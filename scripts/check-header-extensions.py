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

`.hpp` is not forbidden, and the count of them is now CHECKED rather than described. There are
**twelve**: eight vendored (nlohmann ×1, ska_hash ×3, uuid/catch ×4) and four test-local
(`qbm/pgsql/tests/shared/`). This paragraph used to say "the five `.hpp` in the tree" while
naming four correct categories -- the enumeration was right and the numeral was wrong by seven,
and nothing could ever go red on the drift because the number lived in prose and no code path
counted anything. `HPP_CENSUS` below is that enumeration as data: every `.hpp` visited must be
in it (an unrecorded one is a finding) and every entry must still exist (a stale one fails the
run), exactly like `ALLOWED`. The number is now derived from the list instead of typed beside it.

THE FOUR TEST HEADERS ARE A DELIBERATE EXCEPTION, not an oversight. They are not vendored, so
the "renaming a vendored file makes every future vendor drop a merge conflict" argument does not
cover them, and the rule as written -- "`.h` is the only header extension" -- does reach them.
They stay `.hpp` because the rule's PURPOSE does not: this rule exists because three of the eight
retired fragments could not compile alone and were therefore named exclusions in
check-installed-headers.sh, i.e. installed public files permanently outside the self-containment
gate. These four are self-contained GoogleTest fixtures under `tests/`, reached by ~28 includers
in that same tree, installed by nothing, and in no prefix on any platform -- they cannot enter
the failure mode the rule prevents. Renaming them to `.h` is still the cleaner end state and
nothing here argues against it; it is a mechanical change to a module's test tree that closes no
defect, so it is recorded as owed rather than smuggled in. What is NOT acceptable, and is what
this census fixes, is the previous state: an exception that nothing declared and nothing counted.

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

# The `.hpp` census: root-relative path -> reason. CHECKED in BOTH directions, which is the
# whole point -- an `.hpp` that is not here is a finding, and an entry whose file is gone fails
# the run. `.hpp` is allowed, but never silently: the docstring above carried "the five `.hpp`
# in the tree" against a real count of twelve, and no code read that sentence.
#
# Keys are PATH SUFFIXES, not root-relative paths, and that is a correction rather than a taste:
# the first version keyed on the root-relative path like ALLOWED does, which made the same file
# have a different key depending on which root you passed. `dev/agent/header-rules-negative-control.sh`
# runs the check against a copy of `qb/src` alone, where ska_hash's key is
# `qb/vendor/ska_hash/...` and not `src/qb/vendor/ska_hash/...` -- and every vendored .hpp was
# reported unrecorded. The control caught it on the first run after the change. A suffix is
# root-independent and still unambiguous here: no two of these eleven share a tail.
HPP_CENSUS: dict[str, str] = {
    # vendored -- renaming any of these makes every future vendor drop a merge conflict
    # (modules/nlohmann/json.hpp was here until 3.0 stopped vendoring nlohmann; it is now
    #  resolved by find_package / FetchContent and no copy of it exists in the tree)
    "qb/vendor/ska_hash/bytell_hash_map.hpp": "vendored ska_hash (Malte Skarupke)",
    "qb/vendor/ska_hash/flat_hash_map.hpp": "vendored ska_hash (Malte Skarupke)",
    "qb/vendor/ska_hash/unordered_map.hpp": "vendored ska_hash (Malte Skarupke)",
    "qb/vendor/uuid/catch/catch.hpp": "vendored Catch2 v2, stduuid's own test material",
    "qb/vendor/uuid/catch/catch_reporter_automake.hpp": "vendored Catch2 v2 reporter",
    "qb/vendor/uuid/catch/catch_reporter_tap.hpp": "vendored Catch2 v2 reporter",
    "qb/vendor/uuid/catch/catch_reporter_teamcity.hpp": "vendored Catch2 v2 reporter",
    # test-local, qbm/pgsql -- NOT vendored; see the docstring for why they stay .hpp
    "pgsql/tests/shared/pg_integration_fixture.hpp": "test-local GoogleTest fixture, installed by nothing",
    "pgsql/tests/shared/pg_pump.hpp": "test-local GoogleTest fixture, installed by nothing",
    "pgsql/tests/shared/pg_wire_ground_truth.hpp": "test-local GoogleTest fixture, installed by nothing",
    "pgsql/tests/shared/test_config.hpp": "test-local GoogleTest fixture, installed by nothing",
}


def census_key(path: str) -> str | None:
    """The census entry this path matches, or None. Suffix match on a path boundary."""
    norm = path.replace(os.sep, "/")
    for key in HPP_CENSUS:
        if norm == key or norm.endswith("/" + key):
            return key
    return None


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
    census_seen: set[str] = set()
    seen_dirs: set[str] = set()

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
            seen_dirs.add(os.path.dirname(path).replace(os.sep, "/"))
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
            if path.endswith(".hpp"):
                key = census_key(path)
                if key is None:
                    findings.append(
                        f"{path}:1: error: unrecorded '.hpp'. `.h` is the only header "
                        f"extension in qb/qbm since 3.0; `.hpp` survives only for vendored "
                        f"upstreams and the four qbm-pgsql test fixtures. If this is a new "
                        f"qb-authored header, name it `.h`. If it is a vendored drop, add it "
                        f"to HPP_CENSUS with the upstream it belongs to."
                    )
                else:
                    census_seen.add(key)
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

    # Same rule for the census, and it is what turns the number into a gate rather than a
    # sentence. Only entries this invocation could REACH are judged: qb's own CI passes the qb
    # root alone and legitimately cannot see the qbm-pgsql four, and the header-rules control
    # passes a copy of qb/src alone, which reaches the vendored seven but not those four.
    #
    # Reachability is decided by whether the entry's DIRECTORY was walked, not by whether the
    # file exists -- if it were the latter, deleting a census file would read as "not reachable"
    # and the staleness would go unreported, which is precisely the drift this exists to catch.
    census_stale = []
    for key in HPP_CENSUS:
        if key in census_seen:
            continue
        parent = "/" + key.rsplit("/", 1)[0]
        if any(d.endswith(parent) for d in seen_dirs):
            census_stale.append(key)
    if census_stale:
        print(f"::error::stale HPP_CENSUS entr(ies), the directory was swept but the file is "
              f"gone: {' '.join(census_stale)}", file=sys.stderr)
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
