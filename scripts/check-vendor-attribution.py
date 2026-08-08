#!/usr/bin/env python3
"""Fail when a vendored third-party unit is missing its attribution.

Why this exists
---------------
A vendor census found llhttp (~11.6k lines, MIT, Fedor Indutny) shipping with no
LICENSE file and no copyright header in any of its five files, while both
libllhttp.a and the installed public header reached the consumer. nlohmann/json
shipped with an SPDX line but no permission notice and no LICENSE.MIT. And a
verified `cmake --install` of the package preset produced *zero* license files of
any kind -- because both install(DIRECTORY) rules use FILES_MATCHING with
*.h/*.hpp/*.tpp patterns, which installs only files matching a pattern, silently
skipping every LICENSE sitting right next to the headers.

So this checks three things that each failed independently:

  1. ATTRIBUTION   every vendored unit has a license text, or carries the full
                   notice inside every one of its own sources.
  2. INSTALLABLE   that license text is named so the install globs actually copy
                   it. A `COPYING` file is perfectly valid attribution and would
                   still never reach a prefix, which is the bug that started this.
  3. RECORDED      the unit is named in its package's THIRD-PARTY-NOTICES, the
                   file a downstream redistributor reads to discharge their own
                   obligations.

Plus a fourth, aimed at the failure mode nobody notices: a unit that is added to
the tree and never recorded anywhere. The MANIFEST below is authoritative, and
any vendor-shaped directory found on disk but absent from it is an error. That is
how Catch2 v2.13.3 (~19.5k lines, nested inside stduuid) went unrecorded.

Run standalone, or via `bash dev/agent/verify.sh`.
Exit code is non-zero on any finding.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# --- what counts as a license text ------------------------------------------------
# These globs are kept in lockstep with the install() rules in qb/CMakeLists.txt and
# qb/cmake/qbFunctions.cmake. If you add a name here, add it there too, or the file
# will be valid attribution that never reaches an installed prefix.
INSTALLABLE_NOTICE_GLOBS = ("LICENSE", "LICENSE.*", "LICENSE-*", "LICENSE_*", "THIRD-PARTY-NOTICES")

# Names that are real attribution but that the install globs do NOT pick up.
# Matching one of these is a specific, actionable error rather than a bare "missing".
UNINSTALLABLE_NOTICE_NAMES = ("COPYING", "COPYING.txt", "COPYRIGHT", "NOTICE", "LICENCE")

SOURCE_SUFFIXES = (".c", ".h", ".hpp", ".cpp", ".tpp", ".inl", ".cc", ".hh")

COPYRIGHT_RE = re.compile(r"copyright|SPDX-FileCopyrightText|SPDX-License-Identifier", re.I)

# --- the authoritative inventory of vendored units ---------------------------------
# path                       : repo-relative directory of the unit
# notices_in                 : package root whose THIRD-PARTY-NOTICES must name it
# record_as                  : the string that must appear in that THIRD-PARTY-NOTICES
# in_header_notice_ok        : unit has no license FILE but every source carries the
#                              full notice, so the notice travels with the code
MANIFEST = [
    dict(path="qb/src/qb/vendor/qev",          notices_in="qb",
         record_as="src/qb/vendor/qev/"),
    dict(path="qb/src/qb/vendor/nanolog",      notices_in="qb",
         record_as="src/qb/vendor/nanolog/",   in_header_notice_ok=True),
    dict(path="qb/src/qb/vendor/ska_hash",     notices_in="qb",
         record_as="src/qb/vendor/ska_hash/"),
    dict(path="qb/src/qb/vendor/uuid",         notices_in="qb",
         record_as="src/qb/vendor/uuid/"),
    dict(path="qb/src/qb/vendor/uuid/catch",   notices_in="qb",
         record_as="src/qb/vendor/uuid/catch/"),
    # nlohmann/json was here until 3.0 (qb/modules/nlohmann). It is NOT vendored any more -- it is
    # resolved by find_package(nlohmann_json), with a pinned FetchContent fallback -- so it is not a
    # vendored unit and must not be recorded as one. Re-adding a copy of it under any path would be
    # caught by the discovery sweep below, which is the intended behaviour.
    dict(path="qbm/http/not-qb/llhttp",            notices_in="qbm/http",
         record_as="not-qb/llhttp/"),
]

# Vendored files that live OUTSIDE their unit directory because they are the unit's
# single public header, installed on the consumer's include path. These reach a
# consumer as source, so each must carry the notice in-file -- a sibling LICENSE
# elsewhere in the repo does not travel with a header someone copies out.
DETACHED_PUBLIC_HEADERS = [
    "qbm/http/src/qbm/http/vendor/llhttp.h",
]

# A nested directory with these names is a unit's own layout, not a second upstream
# hiding inside the first. Anything else nested one level down has to be declared --
# that is the rule that would have caught Catch2 sitting inside stduuid.
STRUCTURAL_DIRNAMES = {
    "src", "srcs", "source", "sources", "include", "inc", "headers", "lib", "libs",
    "test", "tests", "doc", "docs", "cmake", "build", "examples", "example",
    "benchmark", "benchmarks", "tools", "script", "scripts", "config", "m4", "detail",
}

# Directories that look vendor-shaped but are not vendored upstreams.
DISCOVERY_IGNORE: set[str] = set()


def find_notices(unit: Path) -> list[Path]:
    found: list[Path] = []
    for pattern in INSTALLABLE_NOTICE_GLOBS:
        found.extend(sorted(unit.glob(pattern)))
    return [p for p in found if p.is_file()]


def sources_of(unit: Path) -> list[Path]:
    """Source files directly owned by this unit (not by a nested vendored unit)."""
    nested = [Path(m["path"]) for m in MANIFEST
              if Path(m["path"]) != unit and str(m["path"]).startswith(str(unit) + "/")]
    out = []
    for p in sorted(unit.rglob("*")):
        if not p.is_file() or p.suffix not in SOURCE_SUFFIXES:
            continue
        if any(str(p).startswith(str(n) + "/") for n in nested):
            continue
        out.append(p)
    return out


def discover_units(root: Path) -> set[str]:
    """Vendor-shaped directories on disk, by convention."""
    seen: set[str] = set()
    conventions = list((root / "qb/src/qb/vendor").glob("*"))
    conventions += list((root / "qb/modules").glob("*"))
    for mod in sorted((root / "qbm").glob("*")):
        conventions += list((mod / "not-qb").glob("*"))
    for d in conventions:
        if d.is_dir():
            seen.add(d.relative_to(root).as_posix())
    # one nesting level down, which is how Catch2 hid inside stduuid
    for d in list(conventions):
        if not d.is_dir():
            continue
        for sub in sorted(d.glob("*")):
            if not sub.is_dir() or sub.name.lower() in STRUCTURAL_DIRNAMES:
                continue
            if any(f.suffix in SOURCE_SUFFIXES for f in sub.glob("*")):
                seen.add(sub.relative_to(root).as_posix())
    return seen - DISCOVERY_IGNORE


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    if not (root / "qb").is_dir() or not (root / "qbm").is_dir():
        print(f"error: expected the superproject root, got {root}", file=sys.stderr)
        return 2

    errors: list[str] = []
    checked = 0

    for entry in MANIFEST:
        unit = root / entry["path"]
        rel = entry["path"]
        if not unit.is_dir():
            errors.append(f"{rel}: in MANIFEST but not on disk (deleted? then drop it "
                          f"from MANIFEST and from THIRD-PARTY-NOTICES)")
            continue
        checked += 1

        # 1 + 2 -- attribution present, and named so the install globs copy it
        notices = find_notices(unit)
        if not notices:
            stray = [n for n in UNINSTALLABLE_NOTICE_NAMES if (unit / n).is_file()]
            if stray:
                errors.append(
                    f"{rel}: license text is named {stray[0]!r}, which the install() globs "
                    f"do not match -- it would never reach an installed prefix. "
                    f"Rename it to one of: {', '.join(INSTALLABLE_NOTICE_GLOBS)}")
            elif entry.get("in_header_notice_ok"):
                bare = [p.relative_to(root).as_posix() for p in sources_of(unit)
                        if "permission is hereby granted" not in p.read_text(
                            encoding="utf-8", errors="replace").lower()]
                if bare:
                    errors.append(
                        f"{rel}: declared in-header-notice-only, but these sources carry no "
                        f"permission notice: {', '.join(bare[:4])}")
            else:
                errors.append(
                    f"{rel}: NO license text. Add the upstream license file "
                    f"(one of: {', '.join(INSTALLABLE_NOTICE_GLOBS)}).")
        elif entry.get("in_header_notice_ok"):
            pass  # a file appeared; that is strictly better than the declaration

        # a unit with no license file must have per-file copyright everywhere
        if not notices and not entry.get("in_header_notice_ok"):
            bare = [p.relative_to(root).as_posix() for p in sources_of(unit)
                    if not COPYRIGHT_RE.search(
                        p.read_text(encoding="utf-8", errors="replace")[:4000])]
            if bare:
                errors.append(
                    f"{rel}: {len(bare)} source file(s) carry no copyright line either, "
                    f"e.g. {', '.join(bare[:3])}")

        # 3 -- recorded in the owning package's THIRD-PARTY-NOTICES
        notices_file = root / entry["notices_in"] / "THIRD-PARTY-NOTICES"
        if not notices_file.is_file():
            errors.append(f"{entry['notices_in']}/THIRD-PARTY-NOTICES: missing, but "
                          f"{rel} is vendored under it")
        else:
            text = notices_file.read_text(encoding="utf-8", errors="replace")
            if entry["record_as"] not in text:
                errors.append(
                    f"{rel}: not recorded in {entry['notices_in']}/THIRD-PARTY-NOTICES "
                    f"(expected the string {entry['record_as']!r})")

    # detached public headers must carry the notice in-file
    for rel in DETACHED_PUBLIC_HEADERS:
        p = root / rel
        if not p.is_file():
            errors.append(f"{rel}: listed as a detached public header but not on disk")
            continue
        checked += 1
        head = p.read_text(encoding="utf-8", errors="replace")[:4000]
        if not COPYRIGHT_RE.search(head):
            errors.append(f"{rel}: installed public header of a vendored unit with no "
                          f"copyright notice in the file itself")

    # 4 -- nothing vendored may go unrecorded
    known = {m["path"] for m in MANIFEST}
    for found in sorted(discover_units(root)):
        if found not in known:
            errors.append(f"{found}: vendor-shaped directory not in MANIFEST "
                          f"(add it to {Path(__file__).name} and to THIRD-PARTY-NOTICES)")

    if errors:
        print(f"vendor-attribution: {len(errors)} FINDING(S)")
        for e in errors:
            print(f"  - {e}")
        return 1

    print(f"vendor-attribution: OK ({checked} vendored units, all attributed, "
          f"installable and recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
