#!/usr/bin/env python3
"""check-namespace-scoped-includes.py — no `#include` may be processed inside a namespace.

WHY THIS EXISTS
---------------
An `#include` is a textual splice. Whatever scope the directive sits in is the scope the
included file's declarations land in. So a directive written between the braces of

    namespace qb::pg::detail {
        ...
        #include <fstream>          // <-- here
        ...
    }

does not declare `::std::basic_streambuf`. It declares `qb::pg::detail::std::basic_streambuf`,
and every later unqualified `std::` in that namespace now resolves to a namespace that is
missing everything the rest of the program put in the real one.

The defect is INVISIBLE while some earlier header already pulled the same standard header in:
the include guard fires, the body is skipped, and the misplaced directive is a no-op. It stays
a no-op until the include order changes -- a header dropped, a header added, a different
consumer, a different platform's libc++/libstdc++ split. Nothing warns. The two instances found
in this tree were both in that latent state, and both were only proved live by deleting a
sibling include and watching the same file explode:

  * `qbm/pgsql` `transaction_coro.inl` was spliced into `transaction.inl:526`, BETWEEN the
    braces of `namespace qb::pg::detail`, so its own seven `#include` directives were processed
    in there. Deleting `<fstream>` from the block that had been neutralising them reparsed
    `<fstream>` inside the namespace and produced 20 errors led by `no template named
    'basic_streambuf'; did you mean '::std::basic_streambuf'?`. Fixed in 3.0 by the merge that
    retired the fragment (qbm/pgsql/CHANGELOG.md 3.0.0 "Fixed";
    qbm/pgsql/src/qbm/pgsql/commands.h banner).
  * `qb/src/qb/io/async/tcp/connector.h` carried `<coroutine>`, `<optional>` and `<chrono>` at
    :640-642, inside `namespace qb::io::async::tcp` (opened :69) under `#ifdef
    __cpp_impl_coroutine`. Same latent state, same fix: hoist to the top include block. This one
    SHIPPED: `git show v2.6.0:include/qb/io/async/tcp/connector.h` has the identical three
    directives at the identical :640-642, under the identical namespace opened at :69. So the
    class is not an artefact of the 3.0 restructure — it survived a release.

Both were found by reading. This script is what makes finding them not depend on that.

WHAT IT FORBIDS
---------------
Exactly one thing: an `#include` directive whose enclosing lexical scope stack contains a
`namespace` frame. That is the shape that renames what the included file declares.

WHAT IT DELIBERATELY ALLOWS
---------------------------
  * `extern "C" { #include <time.h> }`. A linkage-specification is NOT a scope. Declarations
    inside it belong to the enclosing namespace exactly as if the braces were not there
    ([dcl.link]), so a C header included in one still declares `::time` -- and doing so is the
    standard idiom for a C header that lacks its own `extern "C"` guard. Seven such sites are
    live in this checkout (qb/src/qb/ev/event.h:41,42,47,48;
    qbm/http .../vendor/llhttp.h:44,581; qbm/http/not-qb/llhttp/include/api.h:35), all correct.
    An `extern "C"` block nested INSIDE a namespace is still reported -- the namespace frame is
    what matters, and the C declarations really would land in it.
  * An `#include` inside a class/struct/union body at global scope: the X-macro member-list
    splice, `struct qev_loop { #define VAR(name, decl) decl; #include "qev_vars.h" };`
    (qb/src/qb/ev/qev.c:2807). The included file is a member-declaration fragment, not
    a header with its own namespace-scope contents, and the splice point is the whole point.
    A class body nested inside a namespace IS reported: use the escape hatch below if a future
    X-macro fragment genuinely needs to live there.

WHAT IT DOES NOT SCAN
---------------------
Vendored trees (`/vendor/`, `/not-qb/`, `/third_party/`). This is a recorded decision, not an
oversight: the vendored Catch2 amalgamation has a real instance of the class at
`qb/src/qb/vendor/uuid/catch/catch.hpp:16049` (`#include <cstddef>` between the braces of
`namespace Catch`, under `#if defined(CATCH_CONFIG_CPP17_BYTE)`), latent for the usual reason.
It is upstream's bug in a single-file drop-in used only by tests, and editing a vendored file
is how every future vendor drop becomes a merge conflict -- the same trade
check-header-extensions.py records for the five vendored `.hpp`. If that fragment is ever
un-vendored or the drop is ever patched for another reason, fix it then and delete this
paragraph.

SCANNER CONFIDENCE (this is a check, not a note)
------------------------------------------------
The verdict for a file is only as good as the brace depth computed for it, so every scanned
file must end at depth 0. One that does not means the masker lost the thread somewhere above,
and any "no findings" for it is unearned -- so it is a hard error, not a warning. Building that
check is what found the three masking bugs this scanner shipped without:

  * RAW STRING LITERALS. `R"({"k":"a\"[[[[[[ b"})"` in
    qb/tests/io/unit/protocol/json-depth-guard.cpp:70 leaked its `{`/`}` into the depth count.
  * DIGIT SEPARATORS. `qb::duration max_delay{std::chrono::milliseconds{30'000}};`
    (qb/src/qb/io/async/coroutine/retry.h:88) -- the `'` read as a character-literal opener,
    which blanked the two closing braces on that line and left the file permanently +2.
  * PARENTHESIS TRACKING, inherited from check-header-linkage.py, which needs it to classify
    declaration heads and this script does not. A paren counter that gets stuck above zero
    stops counting braces entirely and freezes the depth silently. Dropped: braces inside
    parentheses are balanced, so they cannot change whether an include sits in a namespace.

The first two also proved that `qb/docs/doxygen_groups.h` contained `/* handle */` INSIDE a
`/**` block -- which terminates the outer comment early, as clang confirms with `'/*' within
block comment` followed by `error: extraneous closing brace`. That file had not parsed as C++
since before the 2026-06 clang-format pass (which is what mangled the lines after the break).
Fixed alongside this script.

ESCAPE HATCH
------------
    // ns-include: allow <reason>

on the reported line or the line above. The reason is mandatory; a bare `ns-include: allow` is
itself an error, so an exemption always carries an argument someone had to write down. There
are zero uses in the tree and that is the intended number.

ANTI-VACUOUS FLOORS
-------------------
Two, because there are two ways to pass while doing nothing, and the per-root one is PER ROOT
and never shared -- a shared floor lets one root silently absorb another's collapse, which for
a five-root sweep is the difference between a guard and a decoration.

  * `NAME:MIN` per root floors the number of files SCANNED. A moved or renamed path yields zero
    and would otherwise pass.
  * `--min-includes N` floors the number of `#include` directives CLASSIFIED across the whole
    run. A masking bug that blanks the file, or an include regex that stops matching, finds
    zero directives in a thousand files and would also otherwise pass.

Raise a floor when a tree grows; never lower one to make a run pass.

USAGE
-----
    ./scripts/check-namespace-scoped-includes.py                       # qb alone, from qb root
    ./scripts/check-namespace-scoped-includes.py --min-includes 6100 qb:385 qbm/http:216 …
    ./scripts/check-namespace-scoped-includes.py --stats ROOT:MIN      # per-root counts

Exit status: 0 clean, 1 findings, 2 usage/IO/vacuity/confidence error.
"""

from __future__ import annotations

import os
import re
import sys

SCANNED_SUFFIXES = (
    ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tpp", ".inl",
    ".c", ".cc", ".cpp", ".cxx",
)

# Never walked: build output, VCS metadata, tool caches.
SKIP_DIRS = {".git", "build", "__pycache__", ".cache", "node_modules", ".venv"}

# Not our surface. See "WHAT IT DOES NOT SCAN" above -- catch.hpp:16049 is a live instance
# of this class that we are deliberately not editing.
EXCLUDED_PARTS = ("/vendor/", "/not-qb/", "/third_party/")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^">\n]+)[">]', re.M)

ALLOW_RE = re.compile(r"ns-include:\s*allow(?:\s+(?P<reason>\S.*))?$")

# `namespace` last, optionally named, so `QB__NS_INLINE namespace inet {` and
# `namespace qb::io::async::tcp {` and the anonymous `namespace {` all match.
NS_OPEN = re.compile(r"\bnamespace\b\s*(?P<name>[\w:]*)\s*$")
CLASS_KEY = re.compile(r"\b(class|struct|union|enum)\b")
IDENT_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def mask(src: str) -> str:
    """Blank comments, literals and preprocessor lines, preserving every offset and newline.

    Handles what a brace counter has to handle to be trusted: raw string literals, digit
    separators, and backslash-continued preprocessor lines. See SCANNER CONFIDENCE above --
    each of those three is a measured miscount, not a hypothetical one.
    """
    out = list(src)
    n = len(src)
    i = 0
    at_line_start = True

    def blank(lo: int, hi: int) -> None:
        for t in range(lo, min(hi, n)):
            if src[t] != "\n":
                out[t] = " "

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "\n":
            at_line_start = True
            i += 1
            continue
        if at_line_start and c in " \t":
            i += 1
            continue
        if at_line_start and c == "#":
            # A preprocessor line, including every backslash-continued physical line.
            j = i
            while True:
                k = src.find("\n", j)
                if k == -1:
                    k = n
                blank(j, k)
                if src[j:k].rstrip().endswith("\\") and k < n:
                    j = k + 1
                    continue
                i = k
                break
            continue
        at_line_start = False
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            blank(i, n if j == -1 else j)
            i = n if j == -1 else j
            continue
        if c == "/" and nxt == "*":
            # `/*` does not nest: the comment ends at the FIRST `*/`, exactly as the compiler
            # reads it. Matching that is what surfaced qb/docs/doxygen_groups.h.
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            blank(i, j)
            i = j
            continue
        if c in "Rr" and nxt == '"' and not (i and src[i - 1] in IDENT_CHARS):
            # Raw string: R"delim( ... )delim". The delimiter is up to 16 chars, no whitespace,
            # no parens. Braces inside are text, and counting them corrupts every later depth.
            m = re.match(r'R"([^ ()\\\t\n]{0,16})\(', src[i:])
            if m:
                close = ")" + m.group(1) + '"'
                j = src.find(close, i + m.end())
                j = n if j == -1 else j + len(close)
                blank(i, j)
                i = j
                continue
        if c == "'" and i and src[i - 1] in IDENT_CHARS and i + 1 < n and src[i + 1] in IDENT_CHARS:
            # A C++14 digit separator (`30'000`, `0xFF'FF`), not a character literal. Reading it
            # as a quote swallows the rest of the line and everything it was balancing.
            i += 1
            continue
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote:
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            blank(i, j)
            i = j
            continue
        i += 1
    return "".join(out)


def classify_open(head: str) -> tuple[str, str]:
    """What kind of scope does the `{` after this declaration head open? -> (kind, name)."""
    h = re.sub(r"\s+", " ", head).strip()
    # `extern "C" {` / `extern "C++" {`: the literal is masked away, so the head is bare
    # `extern`. A linkage-specification is not a scope ([dcl.link]).
    if h == "extern" or h.endswith(" extern"):
        return "EXTERN_C", ""
    if "(" not in h:
        m = NS_OPEN.search(h)
        if m:
            return "NS", m.group("name") or "(anonymous)"
        if CLASS_KEY.search(h):
            return "CLASS", ""
    return "OTHER", ""


def analyse(path: str) -> tuple[list[tuple[int, str, str]], int, int]:
    """-> (findings, includes_classified, final_depth). A non-zero final_depth means the
    masker lost the thread and this file's verdict is not trustworthy."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    # Include positions come from the RAW text: mask() blanks preprocessor lines, which is
    # exactly what makes the brace depth reliable and what would otherwise erase the subject.
    includes = [(m.start(), m.group(1)) for m in INCLUDE_RE.finditer(src)]

    masked = mask(src)
    lines = src.splitlines()

    stack: list[tuple[str, str, int]] = []  # (kind, namespace name, saved head start)
    head_start = 0
    scope_at: dict[int, list[tuple[str, str]]] = {}
    nxt = 0
    depth = 0

    for i, c in enumerate(masked):
        while nxt < len(includes) and includes[nxt][0] <= i:
            scope_at[includes[nxt][0]] = [(k, nm) for k, nm, _ in stack]
            nxt += 1
        if c == "{":
            kind, name = classify_open(masked[head_start:i])
            stack.append((kind, name, head_start))
            head_start = i + 1
            depth += 1
        elif c == "}":
            depth -= 1
            if stack:
                kind, _, saved = stack.pop()
                # A class body or a braced initialiser sits INSIDE a declaration that
                # continues past the closing brace; a namespace or function body does not.
                head_start = saved if kind in ("CLASS", "OTHER") else i + 1
            else:
                head_start = i + 1
        elif c == ";":
            head_start = i + 1
    while nxt < len(includes):
        scope_at[includes[nxt][0]] = [(k, nm) for k, nm, _ in stack]
        nxt += 1

    findings: list[tuple[int, str, str]] = []
    for off, name in includes:
        frames = scope_at.get(off, [])
        ns = [nm for k, nm in frames if k == "NS"]
        if not ns:
            continue
        line_no = src.count("\n", 0, off) + 1
        state = _exempt(lines, line_no)
        if state is True:
            continue
        if state is None:
            findings.append((line_no, "BARE-ALLOW", "`ns-include: allow` without a reason"))
            continue
        chain = "::".join(ns)
        extra = " (inside a class body in that namespace)" if any(
            k == "CLASS" for k, _ in frames
        ) else ""
        findings.append(
            (line_no, "NS-INCLUDE",
             f"#include <{name}> is processed inside `namespace {chain}`{extra}; it declares "
             f"`{chain}::std`, not `::std`. Hoist it to the top include block, outside the "
             f"namespace (keep any #ifdef guard around it)."),
        )
    return findings, len(includes), depth


def _exempt(lines: list[str], line_no: int) -> bool | None:
    """True = exempt, False = not exempt, None = exemption present but reason missing."""
    for probe in (line_no - 1, line_no - 2):
        if 0 <= probe < len(lines):
            m = ALLOW_RE.search(lines[probe])
            if m:
                return bool(m.group("reason")) or None
    return False


def walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            if not fn.endswith(SCANNED_SUFFIXES):
                continue
            path = os.path.join(dirpath, fn)
            if any(part in "/" + path.replace(os.sep, "/") for part in EXCLUDED_PARTS):
                continue
            yield path


def default_roots() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return [f"{qb_root}:385"]


def main() -> int:
    argv = sys.argv[1:]
    min_includes = 0
    stats = False
    specs: list[str] = []
    while argv:
        a = argv.pop(0)
        if a == "--min-includes":
            if not argv:
                print("usage error: --min-includes needs a number", file=sys.stderr)
                return 2
            try:
                min_includes = int(argv.pop(0))
            except ValueError:
                print("usage error: --min-includes needs a number", file=sys.stderr)
                return 2
        elif a == "--stats":
            stats = True
        elif a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        else:
            specs.append(a)
    specs = specs or default_roots()

    findings: list[str] = []
    unreliable: list[str] = []
    total_files = 0
    total_includes = 0

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

        scanned = 0
        incs = 0
        for path in walk(root):
            scanned += 1
            try:
                found, n_inc, depth = analyse(path)
            except OSError as exc:  # pragma: no cover - IO shape, not logic
                print(f"::error::cannot read {path}: {exc}", file=sys.stderr)
                return 2
            incs += n_inc
            for line_no, kind, text in found:
                findings.append(f"{path}:{line_no}: error: {kind}: {text}")
            if depth != 0:
                unreliable.append(
                    f"{path}: ends at brace depth {depth:+d} -- the masker lost the thread, so "
                    f"this file's verdict is not trustworthy"
                )

        if scanned < floor:
            print(
                f"::error::VACUOUS: {root} yielded {scanned} scanned file(s), floor is {floor}. "
                f"The path is wrong or the tree collapsed; a pass here proves nothing.",
                file=sys.stderr,
            )
            return 2
        if stats:
            print(f"  {root}: {scanned} files, {incs} #include directives")
        total_files += scanned
        total_includes += incs

    if total_includes < min_includes:
        print(
            f"::error::VACUOUS: {total_includes} #include directive(s) classified across "
            f"{total_files} file(s), floor is {min_includes}. The include matcher or the "
            f"masker is broken; a pass here proves nothing.",
            file=sys.stderr,
        )
        return 2

    if unreliable:
        for u in unreliable:
            print(f"::error::CONFIDENCE: {u}", file=sys.stderr)
        print(
            f"\n{len(unreliable)} file(s) could not be scanned reliably. See SCANNER CONFIDENCE "
            f"in {os.path.basename(__file__)}.",
            file=sys.stderr,
        )
        return 2

    if findings:
        for f in findings:
            print(f)
        print(
            f"\n{len(findings)} finding(s). See {os.path.basename(__file__)} for why an "
            f"#include inside a namespace declares `<enclosing-ns>::std`."
        )
        return 1

    print(
        f"OK — {total_files} files scanned across {len(specs)} root(s), {total_includes} "
        f"#include directives classified, all at namespace-free scope; 0 findings."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
