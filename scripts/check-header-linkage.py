#!/usr/bin/env python3
"""check-header-linkage.py — no shipped header may define a STRONG EXTERNAL symbol.

WHY THIS EXISTS
---------------
qb-core is ONE translation unit (`core.cpp` #includes the six real `.cpp`), qb-io is three.
That amalgamation hides two-TU defects *inside* the library while leaving them fully live
against qb's 217 test translation units and every consumer TU. The shape that matters:

    VirtualCore.h  --  Actor.cpp:30 (and VirtualCore.cpp, Main.cpp)  -> into libqb-core.a
                   \\-  the six headers that #include it: qb/actor.h,
                      qb/main.h, qb/patterns.h, qb/core/patterns.h,
                      core/patterns/{discovery,supervisor}.h         -> EVERY consumer TU

Both paths reach the same header. Include guards are PER TU: they stop the double inclusion
inside one TU and do nothing between two TUs, and nothing at all between a consumer TU and
the archive. So one non-template, non-`inline` definition added to such a header is an
instant duplicate symbol. Measured on VirtualCore.h with two consumer TUs + the archive:

    namespace qb { int actor_tpp_helper(int x) { return x + 1; } }
    ->  duplicate symbol 'qb::actor_tpp_helper(int)' in: t2.o / t1.o
        ld: 1 duplicate symbols                      (GNU ld: "multiple definition of ...")

That structure has existed since 2019-02-26 and has never fired, purely because nobody ever
added a non-template: 137 historical revisions of the four `.tpp` lineages, 7.7 years, zero
non-template entities. Through 2.6.0 the `.tpp` extension was the guard, and it was a SOCIAL
one -- the compiler never enforced it. 3.0 retired the extension (see
check-header-extensions.py), which leaves a banner comment. A comment is not a guard. This
script is.

The rule is not scoped to the merge sites, because the hazard is not either: the audit
measured every namespace-scope definition across 288 installed headers and found all of them
template or `inline` (dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md §4.1). That is the invariant
worth freezing, and freezing it whole costs no more than freezing six files.

WHAT IT FORBIDS  (at NAMESPACE scope, in a scanned header)
----------------------------------------------------------
  FUNC-DEF   a function definition that is not a template, not `inline` (nor `QB_MSVC_FORCEINLINE`,
             which is `inline` or MSVC's `__forceinline`), not `constexpr`/`consteval`, not `static`,
             and not in an anonymous namespace.
             Includes an explicit *specialization* -- `template <> void f<int>() {}` is NOT
             implicitly inline and duplicates exactly like a plain function.
  VAR-DEF    a variable definition with external linkage: not `inline`, not `const`/
             `constexpr` (which give internal linkage at namespace scope), not `static`,
             not a bare `extern` declaration, not a template. Includes an explicit
             specialization of a VARIABLE template with an initialiser, which is a
             definition and is not implicitly inline either.

Class scope needs no separate rule for what a class DEFINES: an in-class member function
definition is implicitly `inline`, and the two class-scope shapes that DO emit a strong symbol
-- an out-of-line member definition and an out-of-class static data member definition -- are
written at namespace scope and are caught above. It needs one for what a class body READS,
measured (Huly QB-1246):

  THREAD-WRAP  a `thread_local` static data member DECLARED in a class (not `inline` there) and
             DEFINED `inline` at namespace scope in the same header, named in code between the
             opening brace of its outermost class and that definition -- a member defined in the
             class body (above or below the declaration: a member function body is a complete-class
             context), a nested class, a template or a function written before the definition.
             `listener::current` has to be declared so (its type is its class). On Darwin a
             `thread_local` is reached through a replaceable thread wrapper (`_ZTW...`, whatever its
             initialiser and destructor), and clang gives the wrapper the linkage of the declaration
             the translation unit's first emitted reference names: from such a reference, the in-class
             declaration, which is not `inline` -- the wrapper is emitted `external`, STRONG; from a
             reference written after the definition, `weak_odr hidden`. Two translation units of one
             link whose first reference came from the class are a duplicate
             `thread-local wrapper routine for ...` -- ld's warning on the macOS runner, a link error
             on a developer's Mac: QB-1246 part 3 wrote `listener::release()` in the class, reading
             `current`, and five qbm-http test binaries stopped linking under TSan. Measured with clang
             19 `-target arm64-apple-macos14 -S -emit-llvm` per shape and with Apple clang 21 `nm -m`
             on the runner: a static member, a non-static one, `this->`, a nested class, a template
             written before the definition -- all `external`; the same bodies after it -- `weak_odr
             hidden`; x86_64-linux-gnu -- `weak_odr hidden` in every case (the wrapper is not
             replaceable there, so Linux and Windows never see it). Two shapes are not reported,
             because their wrapper is not strong, measured the same way: a member of a class
             TEMPLATE (the variable is a template instantiation, the wrapper `weak_odr` either way),
             and an UNEVALUATED operand (`sizeof`, `alignof`, `decltype`, `noexcept`: no reference is
             emitted). The cure is the definition's place, not a flag: define the code that reads the
             member AFTER its definition (`listener::release()`, listener.h).

WHAT IT DELIBERATELY ALLOWS
---------------------------
  * `inline` -- it makes the link succeed. It does NOT make the entity single: N inline
    definitions is the identity-duplication class this release spent a step fixing
    (qb/utility/abi.h). Linkable is the bar this check enforces; single is a separate one.
  * anonymous namespaces and namespace-scope `static` -- internal linkage, so no duplicate
    symbol, but one COPY PER INCLUDING TU. That is audit finding R9 (8 helpers in
    qbm/pgsql commands.h, reached by every consumer TU); it is a live, pre-existing finding
    and turning it into a gate failure here would be a different change than this one.
  * an explicit INSTANTIATION definition -- `template int f<int>(int);`. This was a rule
    here until it was measured and was WRONG: an explicit instantiation is still a template
    instantiation and is emitted COMDAT/weak, so it does not duplicate. Two TUs including a
    header carrying one link rc=0, and `nm -m` on the object separates the two cases
    unambiguously:
        weak external  __ZN2qb7inst_fnIiEET_S1_    <- template int inst_fn<int>(int);
        external       __ZN2qb9strong_fnEi         <- int strong_fn(int) { ... }
    An explicit specialization is the opposite -- NOT implicitly inline -- and does
    duplicate (`duplicate symbol 'int qb::spec_fn<int>(int)'`), which is why it is a
    FUNC-DEF above. A rule that fires on something the linker accepts gets silenced with an
    escape hatch and takes the real rules' credibility with it.

KNOWN IMPRECISION
-----------------
This is a scope-aware scanner over comment/string/preprocessor-masked text, not a parser.

  * `struct X { ... } g_obj;` -- a class definition WITH a declarator -- is skipped, because
    the class-key prefix decides first. No such declaration exists in the tree.
  * A namespace-scope function-POINTER variable (`void (*fp)(int) = nullptr;`) reads as a
    function declaration and is skipped.
  * A macro invocation that expands to a definition is invisible: the scanner sees the
    unexpanded token soup.
  * Preprocessor lines are blanked, so a definition inside `#if 0` is still reported.

All four are false NEGATIVES except the last. THREAD-WRAP adds its own, by the same bias:

  * A definition in ANOTHER header than the class is not paired: the rule reads one header at
    a time, and `listener::current` is declared and defined in listener.h.
  * `obj.member` and `ptr->member` count as references whatever `obj` is (a static member read
    through an object IS one; the scanner cannot type `obj`), and `Other::member` does not.

The bias is deliberate: this check is the
cheap, always-on half. The exact half is scripts/check-installed-headers.sh, which compiles
every installed header alone and LINKS every public entry point -- and a link is the only
thing that has ever told the truth here (`-fsyntax-only` and `-c` both pass on the defect).

ESCAPE HATCH
------------
    // header-linkage: allow <reason>

on the reported line or the line above. The reason is mandatory; a bare
`header-linkage: allow` is itself an error, so an exemption always carries an argument
someone had to write down.

ANTI-VACUOUS FLOORS
-------------------
Three, because there are three ways to pass while doing nothing. `NAME:MIN` per root floors the
number of headers SCANNED (a moved path finds zero and passes). `--min-constructs` floors
the number of namespace-scope constructs CLASSIFIED across the whole run (a masking bug that
blanks the file finds zero constructs in a hundred headers and also passes). Floors are PER
ROOT and are never shared -- a shared floor lets one root absorb another's collapse.
`--min-thread-wraps` floors the number of THREAD-WRAP PAIRS found across the run (an in-class
`thread_local` declaration matched with its inline definition): a pairing that stopped
recognising `listener::current`'s definition would check nothing and pass.

USAGE
-----
    ./scripts/check-header-linkage.py                              # qb alone, from qb root
    ./scripts/check-header-linkage.py qb/src:120 qbm/http/src:70 …
    ./scripts/check-header-linkage.py --min-constructs 4000 ROOT:MIN …
    ./scripts/check-header-linkage.py --min-thread-wraps 1 ROOT:MIN …
    ./scripts/check-header-linkage.py --stats ROOT:MIN             # per-root construct counts

Exit status: 0 clean, 1 findings, 2 usage/IO/vacuity error.
"""

from __future__ import annotations

import os
import re
import sys

SCANNED_SUFFIXES = (".h", ".hh", ".hxx", ".ipp", ".tpp", ".inl")

# Not qb's own shipped surface: upstream-derived forks, the nlohmann drop-in, build output, and
# the test trees (which are never installed and whose link errors the build reports directly).
#
# `/qb/ev/` is the event loop. It sat under `/vendor/` until 3.0 and was excluded by that entry;
# promoting it out of vendor/ put 47 findings on the board, and every one of them is this parser
# meeting C it cannot expand. libev declares linkage through its own macros -- `EV_INLINE` is
# `static inline` (qev.h:244) or `static` (:246), `EV_API_DECL` is `static` (:250) or `extern`
# (:252) -- so `EV_INLINE void qev_loop(EV_P_ int flags)` reads to this scanner as a bare
# function definition in a header while being internal-linkage or a declaration in fact. The
# exclusion tracks PROVENANCE, not the name of a directory, exactly as the formatter's does.
# The invariant itself is not waived: the loop is compiled as C into libqev.a and every consumer
# links that archive, so a genuine duplicate symbol there is a link error in qb's own build.
EXCLUDED_PARTS = ("/vendor/", "/qb/ev/", "/modules/", "/build/", "/tests/", "/third_party/",
                  "/.git/")

ALLOW_RE = re.compile(r"header-linkage:\s*allow(?:\s+(?P<reason>\S.*))?$")

CLASS_KEY = re.compile(r"\b(class|struct|union|enum)\b")
# An attribute group that may sit in a class head: `alignas(`, `__declspec(`, `__attribute__((`, `[[`.
ATTR_GROUP = re.compile(r"\balignas\s*\(|\b__declspec\s*\(|\b__attribute__\s*\(|\[\[")
CLASS_PREFIX = re.compile(r"^(class|struct|union|enum)\b")
# `namespace` last, optionally named. Matched by suffix rather than by prefix because qb
# spells its inline namespaces through a macro -- `QB__NS_INLINE namespace inet {`
# (utility/build_macros.h:162) -- and a `^inline?\s*namespace` prefix rule reads that as a
# variable definition, which is how this check produced its first false positive.
NS_OPEN = re.compile(r"\bnamespace\b\s*[\w:]*\s*$")
ANON_NS = re.compile(r"\bnamespace\s*$")
TEMPLATE_ANGLE = re.compile(r"^template\s*<")
EMPTY_TEMPLATE = re.compile(r"^template\s*<\s*>")
EXPL_INST = re.compile(r"^template\s+(?!<)")
# `QB_MSVC_FORCEINLINE` (qb/utility/branch_hints.h) is an inline specifier on every compiler: `__forceinline`
# on MSVC's cl, `inline` on every other compiler, clang-cl included (it defines `__clang__`); both give the
# function inline linkage (qbm-http's origin.h defines one at namespace scope, Huly QB-1186). The whole word
# only: `QB_NOINLINE`, the opposite, spells no inline and leaves a strong symbol.
SAFE_FUNC = re.compile(r"\b(inline|constexpr|consteval|static|friend|QB_MSVC_FORCEINLINE)\b")
SAFE_VAR = re.compile(r"\b(inline|constexpr|const|static|extern|typedef|using|concept)\b")
NOT_A_DECL = re.compile(r"^(namespace|asm|__asm|static_assert)\b")
# `operator=` must not read as the `=` of an initialiser, or every out-of-line assignment
# operator declaration is mistaken for a variable definition.
OPERATOR_RE = re.compile(
    r"\boperator\s*(<=>|==|!=|<=|>=|\+=|-=|\*=|/=|%=|\^=|&=|\|=|<<=|>>=|=|\[\]|\(\)|->\*|->|\+\+|--|<<|>>|[-+*/%^&|~!<>,])"
)

# THREAD-WRAP. A class-scope declaration is a non-inline `thread_local` static data member when it carries
# both words and neither `inline` nor `constexpr` (a `constexpr` static member is implicitly inline).
THREAD_LOCAL = re.compile(r"\bthread_local\b")
STATIC_WORD = re.compile(r"\bstatic\b")
INLINE_OR_CONSTEXPR = re.compile(r"\b(inline|constexpr)\b")
INLINE_WORD = re.compile(r"\binline\b")
# The operators whose operand is unevaluated: a name inside them emits no reference, so no wrapper (measured).
UNEVALUATED_OPEN = re.compile(r"\b(sizeof|alignof|__alignof__|decltype|noexcept)\s*$")
CLASS_HEAD_WORDS = {"class", "struct", "union", "final", "enum"}


def mask(src: str) -> str:
    """Blank comments, string/char literals and preprocessor lines. Offsets are preserved."""
    out = list(src)
    n = len(src)
    i = 0
    at_line_start = True
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
            j = i
            while True:
                k = src.find("\n", j)
                if k == -1:
                    k = n
                for t in range(j, k):
                    out[t] = " "
                if src[j:k].rstrip().endswith("\\") and k < n:
                    j = k + 1
                    continue
                i = k
                break
            continue
        at_line_start = False
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j == -1 else j
            for t in range(i, j):
                out[t] = " "
            i = j
            continue
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for t in range(i, j):
                if src[t] != "\n":
                    out[t] = " "
            i = j
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
            for t in range(i, min(j, n)):
                if src[t] != "\n":
                    out[t] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def norm(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def _strip_trailing_parens(head: str) -> str:
    """Drop one balanced trailing (...) group. `requires(F f, std::function<void()> n)` -> `requires`."""
    h = head.rstrip()
    if not h.endswith(")"):
        return h
    depth = 0
    for idx in range(len(h) - 1, -1, -1):
        if h[idx] == ")":
            depth += 1
        elif h[idx] == "(":
            depth -= 1
            if depth == 0:
                return h[:idx].rstrip()
    return h


def _strip_attribute_groups(head: str) -> str:
    """Drop the attribute groups a class head may carry between its key and its name --
    `alignas(64)`, `__declspec(...)`, `__attribute__((...))`, `[[...]]` -- whose parentheses
    would otherwise read the head as a function's: `struct alignas(64) gate {` is a class
    (found when coroutine/tracking.h's cache-line gate landed, Huly QB-71)."""
    out, i, n = [], 0, len(head)
    while i < n:
        m = ATTR_GROUP.match(head, i)
        if m is None:
            out.append(head[i])
            i += 1
            continue
        if m.group(0).startswith("[["):
            close = head.find("]]", m.end())
            i = n if close < 0 else close + 2
            continue
        depth, j = 0, m.end() - 1  # at the group's opening parenthesis
        while j < n:
            depth += head[j] == "("
            depth -= head[j] == ")"
            j += 1
            if depth == 0:
                break
        i = j
    return "".join(out)


def classify_open(head: str) -> str:
    """What kind of scope does the `{` after this head open?"""
    h = norm(head)
    # A requires-EXPRESSION body. Its braces must not reset the declaration head, or the
    # `template <...>` prefix in front of a constrained definition is lost and the
    # definition reads as a stray non-template.
    if _strip_trailing_parens(h).endswith("requires"):
        return "REQUIRES"
    if ("(" not in h and NS_OPEN.search(h)) or h == "extern":  # `extern "C" {`: literal masked away
        return "NS"
    if CLASS_KEY.search(h) and "(" not in _strip_attribute_groups(h):
        return "CLASS"
    if "(" in h:
        return "FUNC"
    return "OTHER"  # braced initialiser, lambda body, enum-less block


def class_head(head: str) -> tuple[str | None, bool]:
    """The name a class head declares and whether it is a TEMPLATE's:
    `template <typename T> class QB_API name final : public base<T>` -> ("name", True).
    An explicit specialization (`template <>`) declares a class, not a template."""
    h = norm(head)
    templated = False
    if TEMPLATE_ANGLE.match(h):
        templated = not EMPTY_TEMPLATE.match(h)
        depth = 0
        for idx in range(h.index("<"), len(h)):
            depth += h[idx] == "<"
            depth -= h[idx] == ">"
            if depth == 0:
                h = h[idx + 1:]
                break
    h = _strip_attribute_groups(h)
    key = CLASS_KEY.search(h)
    if key is None:
        return None, templated
    rest = re.split(r"(?<!:):(?!:)", h[key.end():], maxsplit=1)[0]  # the base clause goes
    rest = rest.split("<", 1)[0]  # a specialization's arguments go
    names = [t for t in re.findall(r"[A-Za-z_][\w:]*", rest) if t not in CLASS_HEAD_WORDS]
    return (names[-1].rsplit("::", 1)[-1] if names else None), templated


def unevaluated_spans(masked: str, lo: int, hi: int) -> list[tuple[int, int]]:
    """The (open, close) offsets of every parenthesised operand of sizeof/alignof/decltype/noexcept
    in masked[lo:hi]: a name inside one is no reference."""
    spans, opened = [], []
    for k in range(lo, hi):
        c = masked[k]
        if c == "(":
            opened.append((k, bool(UNEVALUATED_OPEN.search(masked[max(lo, k - 32):k]))))
        elif c == ")" and opened:
            start, unevaluated = opened.pop()
            if unevaluated:
                spans.append((start, k))
    spans.extend((start, hi) for start, unevaluated in opened if unevaluated)
    return spans


def thread_wrap_findings(masked: str, members, definitions) -> tuple[list, int]:
    """THREAD-WRAP over one header. `members`: (class, member, window start, declaration's name
    offset) of each in-class non-inline thread_local static data member; `definitions`: (head text,
    first-token offset) of each namespace-scope `inline thread_local` definition. Returns the
    references (offset, class, member, definition offset) named before the definition, and the
    number of pairs found."""
    refs, pairs = [], 0
    for cls, name, lo, decl_at in members:
        target = re.compile(rf"\b{re.escape(cls)}\s*(?:<[^;{{}}]*?>)?\s*::\s*{re.escape(name)}\b")
        hi = next((at for text, at in definitions if at > lo and target.search(text)), None)
        if hi is None:
            continue  # not defined inline in this header: no wrapper of this header's making
        pairs += 1
        spans = unevaluated_spans(masked, lo, hi)
        for m in re.finditer(rf"\b{re.escape(name)}\b", masked[lo:hi]):
            at = lo + m.start()
            if at == decl_at or any(a < at < b for a, b in spans):
                continue
            qualifier = re.search(r"(\w+)\s*(?:<[^;{}]*?>)?\s*::\s*$", masked[max(lo, at - 160):at])
            if qualifier and qualifier.group(1) != cls:
                continue  # `Other::name` is another entity
            refs.append((at, cls, name, hi))
    return refs, pairs


def analyse(path: str):
    """Return (findings, constructs): findings are (line, kind, text); constructs is the number
    of namespace-scope constructs classified, which is what the --min-constructs floor counts."""
    findings, constructs, _pairs = scan(path)
    return findings, constructs


def scan(path: str):
    """`analyse()`, and the number of THREAD-WRAP pairs found, which --min-thread-wraps floors."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    lines = src.splitlines()
    masked = mask(src)

    # (kind, saved head_start, is anonymous namespace, (class name, templated, `{` offset) for a CLASS)
    stack: list[tuple[str, int, bool, tuple[str | None, bool, int] | None]] = []
    tl_members: list[tuple[str, str, int, int]] = []  # THREAD-WRAP: in-class declarations
    tl_definitions: list[tuple[str, int]] = []  # THREAD-WRAP: namespace-scope inline definitions
    head_start = 0
    saw_class_body = False
    saw_braced_init = False
    paren = 0
    findings = []
    constructs = 0

    def at_namespace_scope() -> bool:
        return all(s[0] == "NS" for s in stack)

    def in_anonymous() -> bool:
        return any(s[2] for s in stack)

    def line_of(head_off: int, end_off: int) -> int:
        """Line of the declaration's FIRST TOKEN, not of the offset the head happens to start at.

        A head starts just after the previous `;`/`}`, so it usually opens with a newline, a
        blank line and a doc comment -- all masked to spaces. Reporting that offset points the
        finding (and therefore the `header-linkage: allow` line the reader must write) at a
        blank line, which is exactly the citation defect this release spent a step fixing.
        """
        seg = masked[head_off:end_off]
        skip = len(seg) - len(seg.lstrip())
        return src.count("\n", 0, head_off + skip) + 1

    def exempt(line_no: int) -> bool | None:
        """True = exempt, False = not exempt, None = exemption present but reason missing."""
        for probe in (line_no - 1, line_no - 2):
            if 0 <= probe < len(lines):
                m = ALLOW_RE.search(lines[probe])
                if m:
                    return bool(m.group("reason")) or None
        return False

    i, n = 0, len(masked)
    while i < n:
        c = masked[i]
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif paren == 0 and c == "{":
            head = masked[head_start:i]
            kind = classify_open(head)
            if at_namespace_scope() and kind == "FUNC" and not in_anonymous():
                constructs += 1
                h = norm(head)
                is_template = TEMPLATE_ANGLE.match(h) and not EMPTY_TEMPLATE.match(h)
                if not is_template and not SAFE_FUNC.search(h.split("(")[0]):
                    findings.append((line_of(head_start, i), "FUNC-DEF", h[:160]))
            cls = (*class_head(head), i) if kind == "CLASS" else None
            stack.append((kind, head_start, kind == "NS" and bool(ANON_NS.search(norm(head))), cls))
            head_start = i + 1
        elif paren == 0 and c == "}":
            if stack:
                kind, saved, _, _ = stack.pop()
                if kind in ("REQUIRES", "OTHER", "CLASS"):
                    # These braces sit INSIDE a declaration; the declaration continues.
                    head_start = saved
                    saw_class_body = saw_class_body or kind == "CLASS"
                    saw_braced_init = saw_braced_init or kind == "OTHER"
                else:
                    head_start = i + 1
                    saw_class_body = saw_braced_init = False
            else:
                head_start = i + 1
        elif paren == 0 and c == ";":
            if stack and stack[-1][0] == "CLASS" and not in_anonymous():
                # THREAD-WRAP: a non-inline thread_local static data member of a class at namespace scope
                # (every enclosing scope a namespace or a class, every class named and none a template's).
                raw = masked[head_start:i]
                h = norm(raw)
                if THREAD_LOCAL.search(h) and STATIC_WORD.search(h) and not INLINE_OR_CONSTEXPR.search(h):
                    declarator = re.split(r"[={]", raw, maxsplit=1)[0]
                    name = list(re.finditer(r"[A-Za-z_]\w*", re.sub(r"\[[^\]]*\]\s*$", "", declarator.rstrip())))
                    classes = [s[3] for s in stack if s[0] == "CLASS"]
                    if (name and all(s[0] in ("NS", "CLASS") for s in stack)
                            and all(c3[0] for c3 in classes) and not any(c3[1] for c3 in classes)):
                        tl_members.append((classes[-1][0], name[-1].group(0), classes[0][2], head_start + name[-1].start()))
            if at_namespace_scope() and not in_anonymous():
                h = norm(masked[head_start:i])
                if THREAD_LOCAL.search(h) and INLINE_WORD.search(h):
                    seg = masked[head_start:i]
                    tl_definitions.append((h, head_start + len(seg) - len(seg.lstrip())))
                if h and re.search(r"\w", h):
                    constructs += 1
                    hop = OPERATOR_RE.sub("operatorOP", h)
                    spec = re.split(r"[{=(]", hop)[0]
                    bare = re.sub(r"^template\s*<.*?>\s*", "", h, count=1) if TEMPLATE_ANGLE.match(h) else h
                    kind = None
                    if CLASS_PREFIX.match(bare):
                        pass                       # class/struct/union/enum declaration or definition
                    elif EXPL_INST.match(h):
                        pass                       # explicit INSTANTIATION -- COMDAT/weak, measured
                    elif TEMPLATE_ANGLE.match(h) and not EMPTY_TEMPLATE.match(h):
                        pass                       # template declaration/definition
                    elif (re.search(r"\bextern\b", spec)
                          and (saw_braced_init or re.search(r"^[^(]*=", hop))
                          and not re.search(r"\b(inline|constexpr)\b", spec)):
                        kind = "VAR-DEF"           # initialized extern is a definition, even when const
                    elif SAFE_VAR.search(spec):
                        pass                       # inline / const / constexpr / static / extern / typedef / using
                    elif saw_class_body and not saw_braced_init and not re.search(r"\w", h.rsplit("}", 1)[-1]):
                        pass                       # `} ;` closing a class body
                    elif "(" in hop and hop.index("(") < min(
                        hop.index("{") if "{" in hop else len(hop),
                        hop.index("=") if "=" in hop else len(hop),
                    ):
                        pass                       # function declaration
                    elif NOT_A_DECL.match(h):
                        pass
                    else:
                        kind = "VAR-DEF"
                    if kind:
                        findings.append((line_of(head_start, i), kind, h[:160]))
            head_start = i + 1
            saw_class_body = saw_braced_init = False
        i += 1

    refs, pairs = thread_wrap_findings(masked, tl_members, tl_definitions)
    for at, cls, name, defined_at in refs:
        findings.append((src.count("\n", 0, at) + 1, "THREAD-WRAP",
                         f"`{cls}::{name}` is named before its inline definition at line "
                         f"{src.count(chr(10), 0, defined_at) + 1}: on Darwin its thread wrapper is emitted STRONG"))

    kept = []
    for line_no, kind, text in findings:
        state = exempt(line_no)
        if state is True:
            continue
        if state is None:
            kept.append((line_no, "BARE-ALLOW", "`header-linkage: allow` without a reason"))
            continue
        kept.append((line_no, kind, text))
    return kept, constructs, pairs


def default_roots() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return [f"{os.path.join(qb_root, 'src')}:120"]


def main() -> int:
    argv = sys.argv[1:]
    min_constructs = 0
    min_wraps = 0
    stats = False
    specs: list[str] = []
    while argv:
        a = argv.pop(0)
        if a == "--min-constructs":
            if not argv:
                print("usage error: --min-constructs needs a number", file=sys.stderr)
                return 2
            min_constructs = int(argv.pop(0))
        elif a == "--min-thread-wraps":
            if not argv:
                print("usage error: --min-thread-wraps needs a number", file=sys.stderr)
                return 2
            min_wraps = int(argv.pop(0))
        elif a == "--stats":
            stats = True
        elif a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        else:
            specs.append(a)
    specs = specs or default_roots()

    findings: list[str] = []
    total_headers = 0
    total_constructs = 0
    total_wraps = 0

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

        headers = 0
        constructs = 0
        wraps = 0
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(dirnames)
            for fn in sorted(filenames):
                path = os.path.join(dirpath, fn)
                if not path.endswith(SCANNED_SUFFIXES):
                    continue
                if any(part in path.replace(os.sep, "/") for part in EXCLUDED_PARTS):
                    continue
                headers += 1
                hits, count, pairs = scan(path)
                constructs += count
                wraps += pairs
                for line_no, kind, text in hits:
                    findings.append(
                        f"{path.replace(os.sep, chr(47))}:{line_no}: error: [{kind}] {text}")

        if headers < floor:
            print(
                f"::error::VACUOUS: {root} yielded {headers} headers, floor is {floor}. "
                f"The path is wrong or the tree collapsed; a pass here proves nothing.",
                file=sys.stderr,
            )
            return 2
        if stats:
            print(f"  {root}: {headers} headers, {constructs} namespace-scope constructs, "
                  f"{wraps} thread-local member(s) paired with an inline definition")
        total_headers += headers
        total_constructs += constructs
        total_wraps += wraps

    if total_constructs < min_constructs:
        print(
            f"::error::VACUOUS: {total_constructs} namespace-scope constructs classified across "
            f"{total_headers} headers, floor is {min_constructs}. The masking or the scope walk "
            f"is broken; a pass here proves nothing.",
            file=sys.stderr,
        )
        return 2

    if total_wraps < min_wraps:
        print(
            f"::error::VACUOUS: {total_wraps} in-class thread_local member(s) paired with an inline "
            f"definition across {total_headers} headers, floor is {min_wraps}. The THREAD-WRAP "
            f"pairing is broken or the definition moved; a pass here proves nothing.",
            file=sys.stderr,
        )
        return 2

    if findings:
        for f in findings:
            print(f)
        print(f"\n{len(findings)} finding(s). See {os.path.basename(__file__)}.")
        if any("[THREAD-WRAP]" not in f for f in findings):
            print(
                "A non-template, non-inline definition in a header reached by both the archive and a "
                "consumer TU is a duplicate symbol at the consumer's link. Move it to a .cpp, or make it a "
                "template / `inline`."
            )
        if any("[THREAD-WRAP]" in f for f in findings):
            print(
                "A THREAD-WRAP finding names a thread_local static member before its inline definition: "
                "on Darwin the translation unit whose first reference that is emits the member's thread "
                "wrapper STRONG, and two such units are a duplicate symbol. Define that code after the "
                "member's definition."
            )
        return 1

    print(
        f"OK — {total_headers} headers, {total_constructs} namespace-scope constructs classified, "
        f"{total_wraps} thread-local member(s) paired with an inline definition; "
        f"0 strong external definitions, 0 thread wrappers named before their definition."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
