#!/usr/bin/env python3
"""check-abi-macro-split.py — refuse a build-mode macro that changes qb's ABI.

WHY THIS EXISTS
---------------
Twice in one day a *public type* was found to be selected by a build-mode macro:

  * `qb::unordered_map` picked a different container under `NDEBUG`;
  * `qb::Event::id_type` was `EventId` under `NDEBUG` and `const char *` otherwise,
    which moved `Event::dest` from byte 8 to byte 16.

Both are the same defect. `NDEBUG` is not part of a library's identity — a consumer sets
it from `CMAKE_BUILD_TYPE`, which nothing forces to agree with how the installed
`libqb-core.a` was compiled. When a build-mode macro selects a type, member or alignment
in a *header*, the library and its consumer silently disagree about memory layout. qb's
events are memcpy-relocated across cores and the header is shipped in the install tree,
so the failure mode is silent misrouting or a corrupted read, not a link error.

No CMake gate can catch this: `find_package()` runs before generator expressions are
evaluated, and any `add_compile_options(-UNDEBUG)` placed after it escapes. The only
check that sees the truth is one that reads the header the compiler will actually read.
That is this script.

WHAT IT FORBIDS
---------------
Inside a `#if`/`#ifdef`/`#ifndef`/`#elif` conditional on a build-mode macro
(`NDEBUG`, `_DEBUG`, `DEBUG`) in a scanned header, these are errors:

  1. a type alias           `using X = ...;`  /  `typedef ...;`
  2. a class/struct/union/enum DEFINITION (a forward declaration is fine)
  3. `alignas(...)`, `#pragma pack`, `__declspec(align(...))`
  4. a bare data-member-looking declaration (`Type name;` with no initialiser or `(`)

WHAT IT ALLOWS
--------------
Everything that changes behaviour but not layout: `assert`, `static_assert`, logging,
`fprintf` diagnostics, `constexpr bool` flags, statements, function bodies, and
`#define`s that do not name a type. Those are why the six conditionals qb has today
pass. (They still leave a formal ODR difference on inline functions containing
`assert()` — see dev/analysis/EVENT-ID-ABI-3.0.md §3.4 — but they change no layout.)

KNOWN IMPRECISION
-----------------
Rule 4 is a shape match, not a parse: it cannot tell a class-scope data member from a
local variable, so `std::size_t n;` inside a `#ifndef NDEBUG` block in a *function body*
would be reported too. No header in qb or qbm hits that today (the six conditionals qb
ships are asserts, `fprintf` diagnostics and one `constexpr bool`). The bias is
deliberate — a false positive costs one annotated line, a false negative costs silent
memory corruption in a consumer — and the escape hatch below is how such a line is
resolved.

ESCAPE HATCH
------------
A line may be exempted with a trailing or preceding comment:

    // abi-lint: allow <reason>

The reason is mandatory; a bare `abi-lint: allow` is itself an error, so an exemption
always carries an argument someone had to write down.

USAGE
-----
    ./scripts/check-abi-macro-split.py            # scans src/ from the qb root
    ./scripts/check-abi-macro-split.py DIR [DIR…]

Exit status: 0 clean, 1 findings, 2 usage/IO error.
"""

from __future__ import annotations

import os
import re
import sys

BUILD_MODE_MACROS = ("NDEBUG", "_DEBUG", "DEBUG")

HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".tpp", ".ipp", ".inl")

# Directories that are not qb's own public surface.
EXCLUDED_PARTS = ("/vendor/", "/modules/", "/build/", "/tests/", "/third_party/")

COND_OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)$")
COND_ELIF = re.compile(r"^\s*#\s*(elif|else)\b(.*)$")
COND_CLOSE = re.compile(r"^\s*#\s*endif\b")

MACRO_REF = re.compile(r"\b(" + "|".join(BUILD_MODE_MACROS) + r")\b")

ALLOW = re.compile(r"//\s*abi-lint:\s*allow\b(?P<reason>.*)$")

# Order matters: the first matching rule names the finding, so the most specific diagnosis
# (alignment) is tested before the more general ones. `class alignas(128) X {` satisfies both
# the alignment rule and the type-definition rule; "alignment" is the useful message.
RULES = (
    (
        "alignment directive selected by a build-mode macro",
        re.compile(r"alignas\s*\(|#\s*pragma\s+pack\b|__declspec\s*\(\s*align"),
    ),
    (
        "type alias selected by a build-mode macro",
        re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:using\s+\w+\s*=|typedef\b)"),
    ),
    (
        "type DEFINITION inside a build-mode conditional",
        re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|union|enum)\b(?![^{;]*;\s*$)"),
    ),
    (
        "data member declared inside a build-mode conditional",
        # `Qualified::Type  name;` / `Type *name;` — no '(', no '=', not a statement keyword.
        re.compile(
            r"^\s*(?!return\b|delete\b|throw\b|break\b|continue\b|goto\b|case\b|using\b|friend\b)"
            r"(?:(?:const|volatile|mutable|static|inline|constexpr|unsigned|signed)\s+)*"
            r"[A-Za-z_][\w:]*(?:\s*<[^;()]*>)?(?:\s*[*&]+)?\s+"
            r"[A-Za-z_]\w*\s*(?:\[[^\];]*\])?\s*;\s*$"
        ),
    ),
)


def scan(path: str) -> list[tuple[int, str, str]]:
    """Return [(lineno, rule, source line)] for one file."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError as exc:  # pragma: no cover
        print("check-abi-macro-split: cannot read %s: %s" % (path, exc), file=sys.stderr)
        raise SystemExit(2)

    findings: list[tuple[int, str, str]] = []
    # Stack of booleans: is this conditional nesting level governed by a build-mode macro?
    stack: list[bool] = []
    in_block_comment = False

    for n, raw in enumerate(lines, 1):
        line = raw

        # Strip /* … */ so a commented-out example cannot trip a rule.
        if in_block_comment:
            end = line.find("*/")
            if end < 0:
                continue
            line = line[end + 2 :]
            in_block_comment = False
        start = line.find("/*")
        while start >= 0:
            end = line.find("*/", start + 2)
            if end < 0:
                in_block_comment = True
                line = line[:start]
                break
            line = line[:start] + " " + line[end + 2 :]
            start = line.find("/*")

        m = COND_OPEN.match(line)
        if m:
            stack.append(bool(MACRO_REF.search(m.group(2))))
            continue
        if COND_ELIF.match(line):
            if stack:
                m2 = COND_ELIF.match(line)
                assert m2 is not None
                # `#else` keeps the governing state of its `#if`; `#elif` may add a macro.
                if m2.group(1) == "elif" and MACRO_REF.search(m2.group(2)):
                    stack[-1] = True
            continue
        if COND_CLOSE.match(line):
            if stack:
                stack.pop()
            continue

        if not any(stack):
            continue

        allow = ALLOW.search(raw)
        if allow:
            if allow.group("reason").strip():
                continue
            findings.append((n, "`abi-lint: allow` without a reason", raw.strip()))
            continue

        code = line.split("//", 1)[0]
        if not code.strip():
            continue
        for rule, pattern in RULES:
            if pattern.search(code):
                findings.append((n, rule, raw.strip()))
                break
    return findings


def main(argv: list[str]) -> int:
    roots = argv[1:] or ["src"]
    files: list[str] = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        if not os.path.isdir(root):
            print("check-abi-macro-split: no such path: %s" % root, file=sys.stderr)
            return 2
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in filenames:
                if not name.endswith(HEADER_SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                probe = "/" + path.replace(os.sep, "/").lstrip("./") + "/"
                if any(part in probe for part in EXCLUDED_PARTS):
                    continue
                files.append(path)

    if not files:
        print("check-abi-macro-split: no headers found under %s" % ", ".join(roots),
              file=sys.stderr)
        return 2

    total = 0
    for path in sorted(files):
        for lineno, rule, text in scan(path):
            total += 1
            print("%s:%d: error: %s" % (path, lineno, rule))
            print("    %s" % text)

    if total:
        print()
        print("check-abi-macro-split: %d finding(s) in %d header(s)." % (total, len(files)))
        print("A build-mode macro (%s) must never select a type, a member or an alignment in a"
              % ", ".join(BUILD_MODE_MACROS))
        print("shipped header: a consumer's NDEBUG comes from its own CMAKE_BUILD_TYPE and is")
        print("not required to match the one qb was compiled with, so the two disagree about")
        print("memory layout with no diagnostic. Pick one representation for all build modes,")
        print("or annotate the line `// abi-lint: allow <reason>` if it provably changes no layout.")
        return 1

    print("check-abi-macro-split: OK — %d header(s) scanned, no build-mode macro selects a "
          "type, member or alignment." % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
