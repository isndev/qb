#!/usr/bin/env python3
"""llm-guard.py — anti-drift guard for this project's `llm/` agent-facing docs.

Installed byte-identical at `<project>/scripts/llm-guard.py` in qb, qbm-http, qbm-pgsql and
qbm-redis, exactly as `cite-check.py` is, and driven from the same place: `scripts/doc-lint.sh`,
which each repo's `doc-lint.yml` runs.  `dev/agent/verify.sh` asserts the four copies are
byte-identical, so "byte-identical" is a checked property here rather than a convention.

WHY THIS EXISTS
---------------
`llm/*.llm.md` and `llm/*.llm.api.md` are the reference an agent is told to trust over its own
memory.  They used to live at the qb-dev SUPERPROJECT root, where `dev/agent/llm-guard.py`
covered them; they now live in the public repo each one describes, so that the doc travels with
the code it documents (a tag pins both) and so that each repo is independently indexable
without a private superproject in the loop.

That move opens a gap this script closes.  `dev/agent/llm-guard.py` runs from the superproject,
which is PRIVATE: a commit pushed straight to `isndev/qb` — the repo contributors and CI
actually see — would reach `llm/` with no check at all.  `doc-lint.sh` and `cite-check.py`
cannot stand in for it:

  * `doc-lint.sh`'s `doc_files()` covers `README.md` + `readme/**` and nothing else, and its
    forbidden-token scan matches per LINE with no negation cue (qb and qbm-http have none at
    all; qbm-pgsql and qbm-redis have a coarse removal-context filter).  Measured on today's
    corpus by replaying each repo's own pattern and filter over its own `llm/*.md`: adding
    them to `doc_files()` would flag 11 lines — qb 3, qbm-http 5, qbm-pgsql 2, qbm-redis 1 —
    and every one is a deliberate warn-off ("**NEVER write** `qb::Timestamp` …").  That is the
    doc doing its job, so the FORBIDDEN rule lives here, where the cue can suppress it and the
    suppression is printed.
  * `cite-check.py` asks whether a cited RANGE is inside the file and never reads the lines.
    It is kept and unchanged — but over these eight files the citations are the minority
    surface.  Measured: 2578 declaration-shaped symbol claims against 318 citations.  A
    citation-only checker inspects about an eighth of what is here.

WHAT IT CHECKS  (same rules, same regexes, as the superproject guard — see RELATION below)
------------------------------------------------------------------------------------------
1.  `symbols`   — every declaration-shaped identifier the docs name must exist in THIS repo's
                  source.  This is the coverage that matters; it is the rule that catches
                  "documents API that was removed or renamed".
2.  `citations` — content-aware: for every `file.h:NNN`, the cited line (±`--tolerance`) must
                  actually contain a symbol the citation is about.  An in-range check catches
                  0 of a line-shift drift, because a drifted citation is still inside the file.
2b. `digest`    — the cited lines must still SAY what they said when the citation was last
                  verified (`scripts/llm-cite-digest.baseline`).  Rule 2 is not a substitute:
                  a pure coordinate shift leaves the prose perfectly consistent with the code.
3.  `paths`     — a repo path named in backticks must exist.  Both spellings resolve: the
                  module-relative form an `#include` uses (`src/qbm/http/ws/ws.h`) and the
                  repo-root form the readme books use (`qbm/http/src/qbm/http/ws/ws.h`,
                  405 + 281 + 199 + 635 occurrences across the four books, so it is the
                  house convention and not a mistake to be rewritten).
4.  `forbidden` — retired tokens (`qb::Timestamp` & co.) must not be used affirmatively.  A
                  negation cue on the line, or on the bullet it continues, suppresses; every
                  suppression is PRINTED, never silent.
4b. `phantom`   — names that never existed in any header, same instrument, different list.
5.  `version`   — every `llm/*.md` must carry a `Verified-against:` marker naming THIS repo's
                  qb version, validated BY VALUE against the same source `doc-lint.sh` uses.
                  Before the move no version check reached these files at all: `doc-lint.sh`
                  excluded the superproject's `llm/` surface by construction, and only one of
                  the eight files carried a marker.

CROSS-REPO NAMES — a declared blind spot, not a hidden one
----------------------------------------------------------
A qbm doc legitimately names qb symbols (`qb_load_modules(`, `run_sync(`) and qb's tree is not
present when the module is checked out alone.  Three ways to handle that were considered:

  a. check qb out alongside in each module's `doc-lint.yml`.  Rejected: `doc-lint.sh` already
     states, for the version check, that "this repo is independent and its own CI checks out
     ONLY this repo".  Reversing that here would couple three public repos' doc CI to qb's
     branch state, so a qb `develop` push could redden qbm-redis at random.
  b. resolve qb when it happens to be reachable and skip when it is not.  Rejected outright:
     a rule that silently downgrades to a pass is the exact failure this battery exists to
     catch.
  c. run module-only ALWAYS — one deterministic behaviour everywhere — and record each
     unresolvable name by hand in `scripts/llm-guard.baseline`, which is SELF-CLEANING: an
     entry matching nothing FAILS.  Chosen.

Measured cost of (c), counted from the baselines rather than remembered: **9 symbol entries**
across the four repos (qb 0, qbm-http 6, qbm-pgsql 0, qbm-redis 3), naming **7 distinct symbols**
— `qb_load_modules` is baselined in two qbm-http docs and once in qbm-redis, so entries exceed
names.  (This paragraph said "10 names ... qbm-http 7" and matched neither count.)  Nothing loses
coverage overall — `dev/agent/llm-guard.py` sees both trees and verifies all of them for real;
this script simply states which ones it cannot.

WHY THE PHANTOM AND FORBIDDEN RULES CARRY NO CORPUS FLOOR
---------------------------------------------------------
Every other rule gets an anti-vacuous floor (below).  These two cannot: measured, the phantom
names occur 3 times in qb's two docs and 0 times in all six qbm docs, so a corpus floor is
structurally impossible in three of the four repos and a floor of 0 is not a floor.  A rule
that has stopped matching would then be indistinguishable from a clean corpus.

So liveness is proven directly instead, on every run, by `selftest()`: each FORBIDDEN and
PHANTOM pattern is fed a synthetic line it MUST match and a cue-carrying line it MUST suppress,
and the run aborts if any assertion fails.  That is strictly stronger than a corpus floor — it
holds even when the corpus contains none of the tokens — and it costs microseconds.

ANTI-VACUOUS-PASS
-----------------
A lint that passes because it parsed nothing is worse than no lint.  `FLOORS` below is per
project and per rule; the file stays byte-identical across the four repos because the floors
are keyed by project prefix, the way `cite-check.py`'s `PROSE_FLOOR` already is.  Raise a floor
when a doc grows.  Lowering one is a deliberate act that belongs in a commit message.

RELATION TO `dev/agent/llm-guard.py`
------------------------------------
That script keeps running from the superproject over these same files at their new submodule
paths, with its `--min-llm-docs` floor unchanged at 8.  The overlap is deliberate and the two
scopes differ in a way neither can cover alone: the root guard sees qb + all three modules at
once (so it verifies the 9 baselined cross-repo entries, and it is the only thing that would
catch a qb rename breaking a qbm doc), while this one is the only guard a public repo's own CI
runs.  Neither is redundant; dropping either leaves a real class unchecked.

That second claim was false for three of the four repos until the project identity below stopped
being derived from the checkout's PATH — standalone, this script exited 2 having read nothing.
`dev/agent/standalone-checkout-control.sh` now asserts it in the shape GitHub Actions actually
produces, so it is a checked property rather than a stated one.

Usage:  python3 scripts/llm-guard.py [--stats] [--show N] [--tolerance N]
        python3 scripts/llm-guard.py --record-digests   # after verifying a source move
Exit 0 = clean, 1 = at least one FAILED (or a vacuous run), 2 = cannot determine the version.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys

# --------------------------------------------------------------------------- project identity

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)                       # project root


def relpath(path: str) -> str:
    """Project-relative path, ALWAYS '/'-spelled.

    `os.path.relpath` returns the NATIVE separator, and every path this script compares
    against -- a doc's own citation spec, a `PROJECT_PREFIXES` entry, a baseline key -- is
    written the way the project spells it, with '/'.  On Windows the two never met: doc keys
    read `llm\\qb.llm.md` while the baseline stores `llm/qb.llm.md`, so EVERY baselined
    exception missed, firing twice -- once as the finding it should have suppressed, once as
    `baseline-stale`.  Measured on one tree: 0 such failures on Linux, 34 on Windows.
    Splitting a relpath on `os.sep` (Index.__init__) is the one native use that stays.
    """
    return os.path.relpath(path, ROOT).replace(os.sep, "/")

# --------------------------------------------------------------------------------------------
# WHICH of the four projects is this checkout?  Answered from the tree's CONTENT, never from the
# directory it happens to sit in.  Identical in `cite-check.py` and `gen-llms-txt.py`, so the
# three scripts cannot disagree about what "this project" is.
#
# The rule this replaces read the PATH: `…/qbm/http` -> "qbm/http/", anything else ->
# "<dirname>/".  That is only true inside the qb-dev SUPERPROJECT.  A standalone checkout — the
# only shape a public repo's OWN CI ever sees — is at $GITHUB_WORKSPACE =
# /home/runner/work/<repo>/<repo>, whose parent is named after the REPOSITORY (`qbm-http`),
# never `qbm`.  The derived prefix was therefore `qbm-http/`: a key in none of the per-project
# tables in any of the three scripts.  Measured by running each repo's own `doc-lint.sh` inside
# a GitHub-shaped workspace, in qbm-http, qbm-pgsql and qbm-redis:
#
#   llm-guard.py     FAIL [config], exit 2  — 0 symbols, 0 citations, 0 paths, 0 digests read
#   gen-llms-txt.py  FAIL, exit 1           — the published-index gate checked nothing
#   cite-check.py    exit 0, SILENTLY GREEN — `do_prose = PREFIX in PROSE_ON` was False, so the
#                    prose citation form (23 qbm-http + 37 qbm-pgsql + 331 qbm-redis) was
#                    skipped and its anti-vacuous floor never ran at all
#
# qb was unaffected, because its repository and its superproject directory are BOTH named `qb` —
# which is exactly why four green superproject runs never showed it.
#
# The markers are load-bearing files that cannot be absent from a real checkout and do not move
# when the checkout does: qb is the tree owning `cmake/qbConfig.cmake` (the version source of
# truth VERSIONING.md names), a module is the tree whose `CMakeLists.txt` says
# `project(qbm-<mod> …)`.
PROJECT_PREFIXES = ("qb/", "qbm/http/", "qbm/pgsql/", "qbm/redis/")


def project_prefix(root):
    """This project's prefix in repo-root-form paths, or None if it is none of the four."""
    try:
        with open(os.path.join(root, "cmake", "qbConfig.cmake"), errors="ignore") as fh:
            if re.search(r'^\s*set\(QB_FRAMEWORK_NAME\s+"qb"\)', fh.read(), re.M):
                return "qb/"
    except OSError:
        pass
    try:
        with open(os.path.join(root, "CMakeLists.txt"), errors="ignore") as fh:
            m = re.search(r"^\s*project\(\s*qbm-([A-Za-z0-9_]+)\b", fh.read(), re.M)
    except OSError:
        m = None
    p = ("qbm/%s/" % m.group(1)) if m else None
    return p if p in PROJECT_PREFIXES else None


PREFIX = project_prefix(ROOT)
PROJECT = PREFIX.rstrip("/").replace("/", "-") if PREFIX else "(unidentified)"

# Anti-vacuous floors, PER PROJECT and PER RULE.  Today's measured counts are in the comment
# beside each; the floors sit just under.  `--stats` prints the live numbers.
FLOORS = {
    #            docs  symbols  citations  paths  digests
    "qb/":         (2,     900,        50,     2,      55),   # 2 / 977 /  55 /  3 /  61
    "qbm/http/":   (2,     780,       155,    50,     170),   # 2 / 843 / 169 / 58 / 186
    "qbm/pgsql/":  (2,      95,         9,    28,       9),   # 2 / 107 /  11 / 32 /  11
    "qbm/redis/":  (2,     600,        75,     5,      60),   # 2 / 651 /  83 /  6 /  68
}

SRC_EXT = (".h", ".hpp", ".hh", ".c", ".cpp", ".cc", ".cxx",
           ".cmake", ".txt", ".json", ".in", ".py", ".sh")

# Directories kept out of the IDENTIFIER index: a symbol that exists only in a test or a
# readme snippet is not public API.  File PATHS are still indexed there, so a citation into a
# test resolves.
#
# `scripts` is in this set for a reason found the hard way, and it is the sharpest instrument
# failure this script had.  SRC_EXT includes `.py`, so with `scripts/` indexed THIS FILE feeds
# the identifier set — and its own docstring names `qb_load_modules(` as the example of a
# cross-repo symbol.  The guard therefore confirmed the claim it exists to refute: 3 of the 4
# qbm-http findings and 1 of the 3 qbm-redis findings vanished the moment the script was copied
# into the repos, with no other change.  A first run BEFORE the copy looked correct, which is
# what makes the shape dangerous: the defect is created by installing the guard.
# `llm` is here for the same class one step removed — markdown is not in SRC_EXT today, so the
# docs cannot yet confirm themselves, and this keeps that true if SRC_EXT ever grows.
SKIP_IDENT_DIRS = {".git", "build", "node_modules", ".cache", ".idea", "readme",
                   "tests", "test", "examples", "llm", "scripts"}

# The same self-confirmation hazard as `scripts/` above, one directory up.  `SRC_EXT` holds
# `.txt` (for `CMakeLists.txt`), and `scripts/gen-llms-txt.py` publishes `llms-full.txt` at
# the repo ROOT -- a byte copy of every `llm/` doc.  Indexed, it feeds the identifier set with
# the very names the docs claim, so the published copy CONFIRMS the original.  Measured by
# running the guard with the exclusion removed: 9 of the 12 hand-verified baseline entries go
# STALE -- qbm-http 6 (qb_load_modules, target_link_libraries, TLS_server_method,
# create_server_context, set_supported_alpn_protocols) and qbm-redis 3 (qb_load_modules,
# init, run_sync) -- i.e. the guard stops being able to tell that those names are absent from
# the module, with no source change at all.  These two files are still indexed as PATHS (a
# citation to them resolves); they are only kept out of the identifier set.
SKIP_IDENT_FILES = {"llms.txt", "llms-full.txt"}
SKIP_DIRS = {".git", "build", "node_modules", ".cache", ".idea"}

FORBIDDEN = [
    ("qb::Timestamp", r"qb::Timestamp"),
    ("qb::Duration", r"qb::Duration\b"),
    ("qb::TimePoint", r"qb::TimePoint"),
    ("qb::UtcTimestamp", r"qb::UtcTimestamp"),
    ("qb::LocalTimestamp", r"qb::LocalTimestamp"),
    ("to_timestamp(", r"to_timestamp\("),
    ("to_time_point(", r"to_time_point\("),
    ("<qb/system/timestamp.h>", r"qb/system/timestamp\.h"),
]

# Names that NEVER existed in any header in any version — as opposed to FORBIDDEN above, which
# holds names that were real and got retired.  `EnvelopeFormat` exists as an enum and no
# function consumes it, which is how these looked plausible enough to be written down three
# times across three different surfaces.
PHANTOM = [
    ("ecdh_derive_secret", r"ecdh_derive_secret"),
    ("envelope_encrypt", r"envelope_encrypt"),
    ("envelope_decrypt", r"envelope_decrypt"),
]

# A retired or never-existent name may be NAMED in order to warn an agent off it.  Suppression
# needs one of these cues on the line, or on the bullet it continues (bullets wrap).
# NOTE: no leading \b — the cues include `**no**` and `NO \``, which start on a non-word
# character, and a \b there silently disables them.  IGNORECASE is load-bearing for the same
# reason: `never|NEVER` matches neither "Never" nor "never" in sentence case.
NEGATION = re.compile(
    r"(never|no longer|forbidden|retired|removed|not part of|instead of|"
    r"(?:does not|doesn['’]t|do not|don['’]t|did not|didn['’]t)\s+exists?|"
    r"n(?:one|either)\s+exists?|"
    r"there (?:is|are) (?:\*\*)?no|do not (?:use|emit|write)|"
    r"warn (?:you )?off|not specialized|has \*\*no\*\*|\*\*no\*\*|\bno\s+`|\bno `|"
    r"❌|✗)",
    re.IGNORECASE,
)

# Language keywords + std vocabulary that must never be reported as a missing symbol.
STOP = set("""
if else for while do switch case break continue return goto try catch throw
sizeof alignof alignas decltype typeid static_cast const_cast dynamic_cast reinterpret_cast
co_await co_return co_yield template typename class struct union enum namespace using
public private protected virtual override final friend mutable volatile const constexpr
consteval constinit inline static extern register explicit noexcept nodiscard maybe_unused
true false nullptr this new delete operator requires concept import module export
void bool char wchar_t char8_t char16_t char32_t short int long float double signed unsigned
auto std chrono filesystem string vector map set pair tuple optional variant array span
size_t ssize_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t intptr_t
uintptr_t ptrdiff_t nullptr_t initializer_list function shared_ptr unique_ptr weak_ptr
move forward swap begin end data size empty count find insert emplace erase clear
""".split())

PLACEHOLDER = re.compile(r"^(My|Your|Foo|Bar|Some|Example|Demo)[A-Z]")

FENCE = re.compile(r"^\s*(```|~~~)")
BACKTICK = re.compile(r"`([^`\n]+)`")
CITE = re.compile(r"\b([A-Za-z0-9_][A-Za-z0-9_./+-]*\.(?:h|hpp|cpp|cc|cmake|txt|json))"
                  r":(\d+)(?:-(\d+))?")
CONT = re.compile(r"(?<![\w.])[:/](\d+)(?:-(\d+))?\b")
TAIL = re.compile(r"(?:,\d+(?:-\d+)?)+")
BANNER_LINES = 12
DECL_NAME = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{2,})\s*\(")
# A backticked span that is unmistakably a repo path.  Shape (b) — any slash-separated path
# ending in a letter-initial extension — is what catches a path whose leading segment is
# itself wrong, which a rule keyed on known roots looks straight past.
REPO_PATH = re.compile(
    r"(?:(?:dev|qb|qbm|llm|cmake|examples|scripts|src|tests|readme|\.cursor)/[A-Za-z0-9_.@/+-]+"
    r"|[A-Za-z0-9_.@+-]+(?:/[A-Za-z0-9_.@+-]+)+\.[A-Za-z][A-Za-z0-9]{0,5})")
# The leading segments that make a path a CLAIM ABOUT THIS FAMILY OF REPOS rather than an
# external reference.  `dev` is in the list precisely because it must never resolve in a public
# repo: a published doc pointing into the private superproject is unreachable for its reader,
# and that is what the move had to remove.
IN_FAMILY = re.compile(
    r"^(dev|qb|qbm|llm|cmake|examples|scripts|src|tests|readme|\.cursor)/")
EXAMPLE_LINE = re.compile(r"^\s*[*\-]?\s*(use|usage|e\.g\.|example|note)\s*:", re.I)
ILLUSTRATIVE = re.compile(r"\be\.g\.|\bfor example\b|\bpseudo-?code\b", re.I)
MARKER = re.compile(r"Verified-against")
MARKER_VER = re.compile(r"qb [0-9]+\.[0-9]+\.[0-9]+")

# --------------------------------------------------------------------------- helpers


def expected_version():
    """The qb version every `Verified-against:` marker in this repo must name.

    Same source of truth, and the same hard-stop policy, as `scripts/doc-lint.sh`: qb reads
    `cmake/qbConfig.cmake`, a module reads its own `project(<name> VERSION ...)` because its
    CI checks out only itself.  Not being able to determine it is exit 2, never a skip — a
    lint that quietly passes when it cannot find its expected value is indistinguishable from
    the unvalidated marker it replaces.
    """
    if PROJECT == "qb":
        p, rx = os.path.join(ROOT, "cmake", "qbConfig.cmake"), \
            r'^\s*set\(QB_FRAMEWORK_VERSION\s*"([0-9][0-9.]*)"\)'
    else:
        p, rx = os.path.join(ROOT, "CMakeLists.txt"), \
            r'^\s*project\(' + re.escape(PROJECT) + r'\s+VERSION\s+([0-9][0-9.]*)\)'
    try:
        with open(p, errors="ignore") as fh:
            m = re.search(rx, fh.read(), re.M)
    except OSError:
        m = None
    return (m.group(1) if m else None), relpath(p)


def selftest():
    """Prove the FORBIDDEN / PHANTOM / NEGATION machinery is live, corpus or no corpus.

    See the block above: three of the four repos carry ZERO occurrences of the phantom names,
    so those rules cannot have an anti-vacuous corpus floor.  This asserts the same property
    directly and unconditionally — each pattern must fire on a synthetic affirmative line and
    must be suppressed on a cue-carrying one.  Returns a list of failures; any is fatal.
    """
    bad = []
    for label, pat in FORBIDDEN + PHANTOM:
        probe = f"call {label} here"
        if not re.search(pat, probe):
            bad.append(f"pattern for `{label}` no longer matches its own token")
        if NEGATION.search(probe):
            bad.append(f"negation cue fires on a cue-free line for `{label}`")
        if not NEGATION.search(f"`{label}` was removed; never write it"):
            bad.append(f"negation cue does NOT fire on a warn-off line for `{label}`")
    if not NEGATION.search("There is **no** such call"):
        bad.append("negation cue does not match the `**no**` form")
    if not DECL_NAME.search("`some_call(arg)`"):
        bad.append("DECL_NAME no longer matches a call shape")
    if not CITE.search("see `foo.h:12-30`"):
        bad.append("CITE no longer matches a citation")
    return bad


def digest_of(block):
    """Digest + reviewable excerpt of a cited line range.

    Normalisation strips indentation and blank lines, so a re-indent or a clang-format pass
    does not churn the baseline; anything that changes what the lines SAY does change it.
    The excerpt is what makes a re-record reviewable in `git diff` rather than taken on faith.
    """
    norm = [l.strip() for l in block]
    norm = [l for l in norm if l]
    h = hashlib.sha1("\n".join(norm).encode("utf-8", "replace")).hexdigest()[:12]
    head = (norm[0] if norm else "(blank)").replace("\t", " ")
    return h, (head[:69] + "..." if len(head) > 72 else head)


def strong_shaped(tok: str) -> bool:
    """Is this token certainly code rather than English?  Plain lowercase words match
    somewhere in any file and drove an entire class of false positives."""
    return ("_" in tok) or any(c.isdigit() for c in tok) or tok.isupper() or \
           bool(re.search(r"[a-z][A-Z]", tok)) or tok[:1].isupper()


class Index:
    """Identifier + file index for THIS project's own tree."""

    def __init__(self):
        self.idents = set()
        self.by_base = {}
        for dp, dn, fns in os.walk(ROOT):
            dn[:] = [d for d in dn if d not in SKIP_DIRS]
            rel = os.path.relpath(dp, ROOT)
            idents_ok = not any(part in SKIP_IDENT_DIRS for part in rel.split(os.sep))
            for fn in fns:
                if not fn.endswith(SRC_EXT):
                    continue
                p = os.path.join(dp, fn).replace(os.sep, "/")
                self.by_base.setdefault(fn, []).append(p)
                if not idents_ok or fn in SKIP_IDENT_FILES:
                    continue
                try:
                    with open(p, errors="ignore") as fh:
                        self.idents.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", fh.read()))
                except OSError:
                    pass
        self._lines = {}

    def _candidates(self, spec: str):
        """Every on-disk spelling `spec` may name, most specific first.

        Two forms are in use and both are legitimate.  The module-relative form is how an
        `#include` spells it (`src/qbm/http/ws/ws.h`); the repo-root form is how the readme
        books spell it (`qbm/http/src/qbm/http/ws/ws.h`) and is what `cite-check.py` strips
        with the same PREFIX rule.  Stripping is what makes the repo-root form work when the
        repo is checked out standalone, where there is no `qbm/http/` directory above it.
        """
        spec = spec.lstrip("./")
        out = [os.path.join(ROOT, spec), os.path.join(ROOT, "src", spec)]
        if spec.startswith(PREFIX):
            tail = spec[len(PREFIX):]
            out += [os.path.join(ROOT, tail), os.path.join(ROOT, "src", tail)]
        return out

    def resolve(self, spec: str):
        for c in self._candidates(spec):
            if os.path.isfile(c):
                return [c.replace(os.sep, "/")]
        hits = self.by_base.get(os.path.basename(spec), [])
        if "/" in spec:
            # `spec` is a citation as WRITTEN IN A DOC, always '/'-spelled, while `h`
            # came from os.walk.  Indexing `h` natively made this silently false for
            # every multi-segment spec on Windows -- the candidate list came back EMPTY
            # and the citation was skipped instead of checked.  Hence '/' in by_base.
            hits = [h for h in hits if h.endswith(spec) or
                    h.endswith(spec[len(PREFIX):] if spec.startswith(PREFIX) else spec)]
        return hits

    def resolve_dir(self, spec: str):
        """Docs legitimately name a DIRECTORY the way an #include spells it — "headers under
        `qbm/http/src/qbm/http/ws/`".  `resolve()` is isfile-only, so those need their own
        lookup or every one is reported missing."""
        for c in self._candidates(spec.rstrip("/")):
            if os.path.isdir(c):
                return c.replace(os.sep, "/")
        return None

    def lines(self, path):
        if path not in self._lines:
            try:
                with open(path, errors="ignore") as fh:
                    self._lines[path] = fh.read().split("\n")
            except OSError:
                self._lines[path] = None
        return self._lines[path]


def iter_docs():
    d = os.path.join(ROOT, "llm")
    if os.path.isdir(d):
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".md"):
                yield os.path.join(d, fn)


# --------------------------------------------------------------------------- checks


def check_doc(path, idx, tol, max_occ, want_ver, ver_src,
              findings, stats, suppressions, digests):
    rel = relpath(path)
    with open(path) as fh:
        lines = fh.read().split("\n")

    # ---- 5. Verified-against marker, BY VALUE --------------------------------
    marker = next((l for l in lines if MARKER.search(l)), None)
    if marker is None:
        findings.append((rel, 1, "version",
                         f"no `Verified-against:` marker — an indexer cannot tell which "
                         f"release this file describes; expected one naming qb {want_ver}",
                         None))
    else:
        found = MARKER_VER.findall(marker)
        if not found:
            findings.append((rel, 1, "version",
                             f"`Verified-against:` names no qb version "
                             f"(expected qb {want_ver}): {marker.strip()[:100]}", None))
        elif found[-1].split()[1] != want_ver:
            findings.append((rel, 1, "version",
                             f"`Verified-against:` says {found[-1]}, but {ver_src} says "
                             f"qb {want_ver}", None))
        else:
            stats["markers"] += 1

    in_fence = False
    for i, line in enumerate(lines, 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue

        # A warn-off bullet can wrap, so look back to the start of it, capped at 3 lines.
        prev = "\n".join(lines[max(0, i - 4):i - 1])
        negated = bool(NEGATION.search(line) or NEGATION.search(prev))

        # ---- 4 / 4b. retired + never-existent tokens -------------------------
        for label, pat, kind, why in (
                [(l, p, "forbidden", "retired token") for l, p in FORBIDDEN] +
                [(l, p, "phantom", "name that never existed in any header")
                 for l, p in PHANTOM]):
            if not re.search(pat, line):
                continue
            stats[kind] += 1
            if negated:
                suppressions.append((rel, i, f"{label} [{kind} warn-off]"))
            else:
                findings.append((rel, i, kind,
                                 f"`{label}` is a {why} and is used affirmatively here "
                                 f"(no negation cue on this line or the one above)", None))

        spans = BACKTICK.findall(line)

        # ---- 3. repo paths named in backticks must exist ---------------------
        if not in_fence:
            for span in spans:
                m = REPO_PATH.fullmatch(span.strip())
                if not m:
                    continue
                rp = m.group(0)
                stats["paths"] += 1
                if idx.resolve(rp) or idx.resolve_dir(rp):
                    continue
                # A path rooted in THIS FAMILY of repos is a claim about them and must
                # resolve.  Everything else gets an escape hatch, because a doc naming a
                # third-party or illustrative path (`nlohmann/json.hpp`) is not asserting
                # anything about this tree.
                #
                # That hatch used to be the only test, and it swallowed exactly the class it
                # was surrounded by: it asked whether `os.path.join(ROOT, dirname(rp))` is a
                # directory, with no PREFIX stripping, so inside a standalone checkout
                # `qbm/pgsql/src/qbm/pgsql/gone.h` resolves its dirname to
                # `<pgsql-root>/qbm/pgsql/src/...` — absent — and was written off as external.
                # Every wrong repo-root-form path passed, and so did a reintroduced
                # `dev/docs-overhaul/...`, which is the precise defect the move removed.
                # Found by llm-shipped-guard-negative-control.sh; both controls now sit in it.
                if not IN_FAMILY.match(rp) and "/" in rp \
                        and idx.resolve_dir(os.path.dirname(rp)) is None:
                    stats["path_skip_external"] += 1
                    continue
                findings.append((rel, i, "paths", f"`{rp}` does not exist in {PROJECT}", None))

        # ---- 1. declaration-shaped symbol existence --------------------------
        if not in_fence and not EXAMPLE_LINE.match(line.strip()):
            for span in spans:
                if "(" not in span:
                    continue
                for m in DECL_NAME.finditer(span):
                    name = m.group(1)
                    if name in STOP or PLACEHOLDER.match(name):
                        continue
                    stats["symbols"] += 1
                    if name in idx.idents:
                        continue
                    if negated or ILLUSTRATIVE.search(line):
                        suppressions.append((rel, i, f"{name}( [warn-off/illustrative]"))
                        continue
                    findings.append((rel, i, "symbols",
                                     f"`{name}(` documented but absent from {PROJECT} — if it "
                                     f"belongs to another repo, record it in "
                                     f"scripts/llm-guard.baseline",
                                     f"{rel}\tsymbol:{name}"))

        # ---- 2. content-aware citations --------------------------------------
        cites = []
        full = [(m.start(), m.end(), m.group(1)) for m in CITE.finditer(line)]
        for s, e, spec in full:
            m = CITE.match(line, s)
            cites.append((spec, int(m.group(2)), int(m.group(3) or m.group(2)), e, s))
        if full:
            consumed = {c[3] for c in cites}
            for m in CONT.finditer(line):
                if m.end() in consumed:
                    continue
                prior = [p for s, e, p in full if e <= m.start()]
                if not prior:
                    continue
                if m.start() > 0 and line[m.start() - 1] not in " ,;(`" and \
                        not (line[m.start()] == "/" and line[m.start() - 1].isdigit()):
                    continue
                cites.append((prior[-1], int(m.group(1)),
                              int(m.group(2) or m.group(1)), m.end(), m.start()))
            # A comma-joined tail binds to the SAME path (`ActorId.h:401,442`).  CONT cannot
            # see it — it needs a `:` or `/` before the digits — so without this every such
            # tail is silently unchecked.
            for s, e, p in full:
                t = TAIL.match(line, e)
                if not t:
                    continue
                for part in t.group(0).lstrip(",").split(","):
                    a, _, b = part.partition("-")
                    cites.append((p, int(a), int(b or a), e, s))

        if not cites:
            continue

        # ---- 2b. content digest, for EVERY cited range -----------------------
        # Collected before the anchor logic below, which stops at the first target that
        # confirms and would leave the rest of a multi-target line un-digested.
        if digests is not None:
            for spec, lo, hi, _, _ in cites:
                cands = [c for c in idx.resolve(spec) if idx.lines(c) is not None]
                if len(cands) != 1:
                    stats["digest_skip_ambiguous"] += 1
                    continue
                src = idx.lines(cands[0])
                if hi > len(src) or lo < 1 or lo > hi:
                    continue            # out of bounds: rule 2 already fails it, loudly
                digests[(rel, f"{spec}:{lo}-{hi}")] = digest_of(src[lo - 1:hi])

        anchors = set()
        for span in spans:
            for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", span):
                if tok in STOP or not strong_shaped(tok):
                    continue
                if re.match(r"^[A-Za-z0-9_.+-]+\.(h|hpp|cpp|cc|cmake|txt|json)$", tok):
                    continue
                anchors.add(tok)

        # The corpus overwhelmingly writes `symbol` (`file.h:NNN`), so the identifier just
        # before a citation is that citation's subject.  Using it restores per-target
        # precision on a line citing several files, where a first target that confirms would
        # otherwise mask a drift in the others.
        subj = {}
        for _, _, _, _, st in cites:
            for span in reversed(re.findall(r"`([^`\n]+)`", line[:st])):
                if CITE.search(span) or re.search(r"\.(h|hpp|cpp|cc|cmake|txt|json)\b", span):
                    continue                              # a citation span names no subject
                cand = [x for x in re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", span)
                        if x not in STOP]
                if cand:
                    subj[st] = cand[-1]
                    break

        line_confirmed, line_checked, misses = False, False, []
        for spec, lo, hi, _, st in cites:
            cands = [c for c in idx.resolve(spec) if idx.lines(c) is not None]
            if not cands:
                stats["cite_skip_unresolved"] += 1
                continue
            stats["citations"] += 1

            # An anchor equal to the cited file's own name matches that file's banner comment
            # and would confirm any citation into it.  Do not drop the stem — a citation is
            # often ABOUT the class the file is named for — drop only its banner occurrences.
            stem = os.path.splitext(os.path.basename(spec))[0]
            noise = set(spec.replace("/", ".").split(".")) - {stem}
            local = {a for a in anchors if a not in noise}
            primary = ({subj[st]} if st in subj and subj[st] not in noise else None)
            banner = {stem, stem.replace("_", "")}

            in_bounds = [c for c in cands if hi <= len(idx.lines(c))]
            if not in_bounds:
                best = max(cands, key=lambda c: len(idx.lines(c)))
                findings.append((rel, i, "citations",
                                 f"{spec}:{lo}-{hi} is outside the file "
                                 f"({len(idx.lines(best))} lines)", None))
                continue                                  # out of bounds is never baselineable

            lo_w, hi_w = lo - tol, hi + tol
            for tgt in in_bounds:
                src = idx.lines(tgt)
                confirmed_here, usable = False, []
                for pass_set in ([primary, local] if primary else [local]):
                    usable = []
                    for a in pass_set:
                        rx = re.compile(r"\b" + re.escape(a) + r"\b")
                        occ = [n for n, l in enumerate(src, 1) if rx.search(l)]
                        if a in banner:
                            occ = [n for n in occ if n > BANNER_LINES]
                        if occ and len(occ) <= max_occ:
                            usable.append((a, occ))
                    if usable and any(any(lo_w <= n <= hi_w for n in occ) for _, occ in usable):
                        confirmed_here = True
                        break
                # STEM-ONLY is UNJUDGEABLE, not wrong.  A stem locates the FILE, not a
                # position inside it, so refuting a range with it is guessing; rule 2b covers
                # every one of these for drift, which is the class actually gated.
                if usable and not [a for a, _ in usable if a not in banner]:
                    stats["cite_skip_stem_only"] += 1
                    continue
                if not usable:
                    continue
                line_checked = True
                if confirmed_here:
                    line_confirmed = True
                    break
                near = sorted(usable, key=lambda p: min(abs(n - lo) for n in p[1]))[:3]
                msg = (f"{spec}:{lo}{'-' + str(hi) if hi != lo else ''} holds none of them — "
                       + "; ".join(f"`{a}` is at :{','.join(map(str, occ[:3]))}"
                                   for a, occ in near))
                if st in subj:
                    findings.append((rel, i, "citations", msg, f"{rel}\t{spec}:{lo}-{hi}"))
                    line_confirmed = True
                else:
                    misses.append((msg, f"{rel}\t{spec}:{lo}-{hi}"))
            if line_confirmed:
                break

        if not line_checked:
            stats["cite_skip_no_anchor"] += 1
        elif not line_confirmed and misses:
            findings.append((rel, i, "citations", misses[0][0], misses[0][1]))


# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=f"anti-drift guard for {PROJECT}'s llm/ docs")
    ap.add_argument("--tolerance", type=int, default=4,
                    help="lines of slack around a cited range (default 4)")
    ap.add_argument("--max-occ", type=int, default=40,
                    help="an anchor occurring more often than this locates nothing")
    ap.add_argument("--show", type=int, default=40)
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--baseline", default=os.path.join(SCRIPT_DIR, "llm-guard.baseline"))
    ap.add_argument("--digest-baseline",
                    default=os.path.join(SCRIPT_DIR, "llm-cite-digest.baseline"))
    ap.add_argument("--no-baseline", action="store_true",
                    help="ignore the baseline; report every finding (use to re-derive it)")
    ap.add_argument("--record-digests", action="store_true",
                    help="rewrite llm-cite-digest.baseline from the tree. DELIBERATE ACT: "
                         "review the resulting `git diff` — each line carries the cited text, "
                         "so a drift you are about to baptise is visible there.")
    # Floor overrides.  CI passes none of these, so the FLOORS table above is what gates a real
    # run.  They exist because an anti-vacuous floor that cannot be raised from outside cannot
    # be NEGATIVELY CONTROLLED — there would be no way to prove the floor fires, and an
    # unprovable guard is the thing this whole battery exists to refuse.
    # `dev/agent/llm-shipped-guard-negative-control.sh` drives each one.
    for _f in ("docs", "symbols", "citations", "paths", "digests"):
        ap.add_argument(f"--min-{_f}", type=int, default=None,
                        help=f"override the {_f} floor (negative controls only)")
    a = ap.parse_args()

    if PREFIX is None or PREFIX not in FLOORS:
        print(f"  FAIL [config] cannot identify which project {ROOT} is: it has neither a "
              f"cmake/qbConfig.cmake setting QB_FRAMEWORK_NAME \"qb\" nor a CMakeLists.txt "
              f"with project(qbm-<mod> …) naming one of {sorted(FLOORS)}. Guessing would make "
              f"every floor here meaningless, so this is a hard stop rather than a skip.")
        return 2

    bad = selftest()
    if bad:
        for b in bad:
            print(f"  FAIL [selftest] {b}")
        print("  the rule machinery is broken; every PASS below would be meaningless")
        return 1

    want_ver, ver_src = expected_version()
    if want_ver is None:
        print(f"  FAIL [version] cannot read the qb version from {ver_src}")
        print(f"  refusing to validate Verified-against markers against an unknown version")
        return 2

    accepted = {}
    if not a.no_baseline and os.path.isfile(a.baseline):
        with open(a.baseline) as fh:
            for raw in fh:
                raw = raw.rstrip("\n")
                if not raw.strip() or raw.lstrip().startswith("#"):
                    continue
                parts = raw.split("\t")
                if len(parts) >= 2:
                    accepted["\t".join(parts[:2])] = "\t".join(parts[2:]) or "(no reason)"

    findings, suppressions, digests = [], [], {}
    stats = {k: 0 for k in ("symbols", "citations", "paths", "markers", "forbidden",
                            "phantom", "path_skip_external", "cite_skip_unresolved",
                            "cite_skip_no_anchor", "cite_skip_stem_only",
                            "digest_skip_ambiguous")}

    idx = Index()
    docs = list(iter_docs())
    for d in docs:
        check_doc(d, idx, a.tolerance, a.max_occ, want_ver, ver_src,
                  findings, stats, suppressions, digests)

    min_docs, min_sym, min_cit, min_paths, min_dig = FLOORS[PREFIX]
    min_docs = a.min_docs if a.min_docs is not None else min_docs
    min_sym = a.min_symbols if a.min_symbols is not None else min_sym
    min_cit = a.min_citations if a.min_citations is not None else min_cit
    min_paths = a.min_paths if a.min_paths is not None else min_paths
    min_dig = a.min_digests if a.min_digests is not None else min_dig

    if a.record_digests:
        # Recording is where a vacuous run does the most damage: a parser that has stopped
        # matching would overwrite the baseline with nothing, and the next check would compare
        # nothing to nothing and pass.  Floor the RECORD as hard as the check.
        if len(digests) < min_dig:
            print(f"  FAIL [vacuous] refusing to record {len(digests)} digests "
                  f"(expected >= {min_dig}); the parser has stopped matching the corpus and "
                  f"recording now would erase the baseline")
            return 1
        with open(a.digest_baseline, "w") as fh:
            fh.write(
                "# llm-cite-digest.baseline -- what every line-cited range in llm/ SAID when\n"
                "# it was last verified.  Regenerate with:\n"
                "#     python3 scripts/llm-guard.py --record-digests\n"
                "# and REVIEW the git diff: the trailing excerpt is the first cited line, so a\n"
                "# citation that has silently drifted onto other code shows up as a changed\n"
                "# excerpt rather than as an opaque hash.  Fields: doc <TAB> cited-spec <TAB>\n"
                "# digest <TAB> excerpt.  Keyed WITHOUT the doc's own line number, so\n"
                "# re-flowing a paragraph does not re-key the file.\n")
            for (doc, spec), (h, head) in sorted(digests.items()):
                fh.write(f"{doc}\t{spec}\t{h}\t{head}\n")
        print(f"  recorded {len(digests)} digests -> "
              f"{relpath(a.digest_baseline)}")
        return 0

    # -- rule 2b: the cited lines must still say what they said -------------------
    recorded = {}
    if os.path.isfile(a.digest_baseline):
        with open(a.digest_baseline) as fh:
            for raw in fh:
                if not raw.strip() or raw.lstrip().startswith("#"):
                    continue
                p = raw.rstrip("\n").split("\t")
                if len(p) >= 3:
                    recorded[(p[0], p[1])] = (p[2], p[3] if len(p) > 3 else "")
    dig_drift, dig_new = [], []
    for key, (h, head) in sorted(digests.items()):
        if key not in recorded:
            dig_new.append((key, head))
        elif recorded[key][0] != h:
            dig_drift.append((key, recorded[key][1], head))
    dig_stale = sorted(set(recorded) - set(digests))

    # -- anti-vacuous-PASS floors ------------------------------------------------
    vacuous = []
    if len(docs) < min_docs:
        vacuous.append(f"parsed {len(docs)} llm/ docs, expected >= {min_docs}")
    if stats["markers"] < min_docs and not [f for f in findings if f[2] == "version"]:
        vacuous.append(f"validated {stats['markers']} Verified-against markers with no "
                       f"version finding to explain it, expected >= {min_docs}")
    if stats["symbols"] < min_sym:
        vacuous.append(f"checked {stats['symbols']} symbols, expected >= {min_sym}")
    if stats["citations"] < min_cit:
        vacuous.append(f"checked {stats['citations']} citations, expected >= {min_cit}")
    if stats["paths"] < min_paths:
        vacuous.append(f"checked {stats['paths']} repo paths, expected >= {min_paths}")
    if len(digests) < min_dig:
        vacuous.append(f"digested {len(digests)} cited ranges, expected >= {min_dig}")

    if a.stats or suppressions:
        print(f"  {PROJECT}: docs={len(docs)} markers={stats['markers']}/{len(docs)} "
              f"symbols={stats['symbols']} citations={stats['citations']} "
              f"paths={stats['paths']} digests={len(digests)} "
              f"forbidden-hits={stats['forbidden']} phantom-hits={stats['phantom']} "
              f"skipped(unresolved={stats['cite_skip_unresolved']}, "
              f"no-anchor={stats['cite_skip_no_anchor']}, "
              f"stem-only={stats['cite_skip_stem_only']}, "
              f"ambiguous-digest={stats['digest_skip_ambiguous']}, "
              f"external-path={stats['path_skip_external']})  "
              f"deliberate warn-offs suppressed={len(suppressions)}")
    if a.stats:
        for rel, ln, tok in suppressions:
            print(f"    suppressed  {rel}:{ln}  {tok}")

    # Baselined findings are known-good exceptions, verified by hand.  The key carries the
    # cited coordinates (or the symbol name), so a citation that drifts to a NEW wrong line no
    # longer matches its entry and fails.
    used, kept = set(), []
    for f in findings:
        if f[4] is not None and f[4] in accepted:
            used.add(f[4])
            continue
        kept.append(f)
    stale = sorted(set(accepted) - used)
    findings = kept

    by_kind = {}
    for f in findings:
        by_kind[f[2]] = by_kind.get(f[2], 0) + 1

    for rel, ln, kind, msg, _k in findings[:a.show]:
        print(f"  FAIL [{kind}] {rel}:{ln}: {msg}")
    if len(findings) > a.show:
        print(f"  ... and {len(findings) - a.show} more")

    # An entry matching nothing is itself a failure: a stale allowlist silently widens what the
    # gate ignores, which is the vacuous-pass trap wearing a different hat.
    for k in stale:
        print(f"  FAIL [baseline-stale] {k.replace(chr(9), '  ')} — this entry matches no "
              f"finding; the citation or symbol was fixed, moved or removed. Delete the line "
              f"from {relpath(a.baseline)}.")

    for (doc, spec), was, now in dig_drift[:a.show]:
        print(f"  FAIL [digest] {doc}: {spec} no longer says what it said")
        print(f"                  was: {was}")
        print(f"                  now: {now}")
    if len(dig_drift) > a.show:
        print(f"  ... and {len(dig_drift) - a.show} more drifted")
    for (doc, spec), head in dig_new[:a.show]:
        print(f"  FAIL [digest-new] {doc}: {spec} has never been verified — check it points at "
              f"the right lines, then re-record. now: {head}")
    if len(dig_new) > a.show:
        print(f"  ... and {len(dig_new) - a.show} more unrecorded")
    for doc, spec in dig_stale[:a.show]:
        print(f"  FAIL [digest-stale] {doc}: {spec} matches no citation — the citation was "
              f"changed or removed; re-record.")
    if len(dig_stale) > a.show:
        print(f"  ... and {len(dig_stale) - a.show} more stale")
    if dig_drift or dig_new or dig_stale:
        print(f"  digest: {len(dig_drift)} drifted, {len(dig_new)} unrecorded, "
              f"{len(dig_stale)} stale of {len(digests)} — after verifying each by hand, "
              f"re-record with: python3 scripts/llm-guard.py --record-digests")

    if accepted and not a.no_baseline:
        print(f"  baselined: {len(used)}/{len(accepted)} "
              f"(see {relpath(a.baseline)})")

    if vacuous:
        for v in vacuous:
            print(f"  FAIL [vacuous] {v} — the parser has stopped matching the corpus; "
                  f"a PASS here would be meaningless")
        return 1
    if stale or dig_drift or dig_new or dig_stale:
        return 1
    if findings:
        print("  " + "  ".join(f"{k}={v}" for k, v in sorted(by_kind.items())))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
