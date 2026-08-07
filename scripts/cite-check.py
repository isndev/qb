#!/usr/bin/env python3
"""cite-check.py — citation-integrity guard (installed at <project>/scripts/).

Validates every citation in this project's Markdown:
  * the cited file exists, and
  * every cited line range falls within the file's current length.

Resolves PATHs that belong to THIS project (module-relative like `tests/...`,
`commands/foo.h`, or the repo-root form `qbm/redis/...`). Citations that point at
a *different* project are skipped — each project's own guard checks its files, and
in CI a module is checked out in isolation without its siblings.

Exit 0 = clean, 1 = at least one missing file or out-of-bounds range.
Run from the project root:  python3 scripts/cite-check.py

THE PROSE FORM
--------------
The three `src:`-marked forms below were, for a long time, the only ones parsed — and all
three require the literal token `src:`.  The readme books also cite in PROSE, with no
marker at all:

    ... throws `"Not enough names in row data extraction"` (`resultset.h:957`).
    ... the coroutine path (`src/qbm/pgsql/commands.h:1309,1337,1412`).
    ... `geosearch`'s callback overload (geo_commands.h:105-109) is the lone exception.

The second contains `src/` but not `src:`, so it missed the parenthetical form too.  These
look exactly like checked citations and were completely invisible — **1010 of them** across
the four books, more than a third of the whole corpus.  Two real defects hid there:
`qbm/pgsql/readme/results.md` cited `transaction_coro.inl:96,128,211` for three calls whose
real lines were 104/132/207 (line 96 was BLANK, line 211 a bare `}`), and the same file
cited `resultset.h:956` after an edit one line above had moved the target to `:957`.
`verify.sh` was green through both.

`dev/agent/llm-guard.py` parses this form and covers the books from the SUPERPROJECT root,
but it runs from there — and this script is what each module's own `doc-lint.yml` invokes,
so a qbm module built standalone in its own CI has no llm-guard.  That gap is what the
prose form closes here.

NEGATION.  A related linter produced 11 false positives on `qbm/redis` by matching backticked
IDENTIFIERS inside *Pitfalls* blocks that correctly assert a name does not exist, and the fix
there was a negation cue.  A cue is the WRONG instrument for a citation, and it was measured
before being rejected: `llm-guard.py`'s NEGATION pattern fires on **144 of these 734
citations (20%)** — `qb` 108, `qbm/http` 7, `qbm/redis` 29 — and every one is a BEHAVIOURAL
"never"/"no longer" whose citation is still a live positional claim ("Actors **never** migrate
between cores (`src/qb/core/VirtualCore.h:172`)").  A cue would blind this guard on one
citation in five, including 6 real citations that live inside redis's own Pitfalls blocks.
The discriminator used instead is structural: **a citation must carry `file:line`**.  A mere
name mention does not, so the entire false-positive class (`FLUSHALL`, `HMSET`, `Reply<...>`)
cannot match — measured at 0 hits — and the polarity of the surrounding sentence is
irrelevant, which is the point.  Nothing in the corpus cites a deliberately-absent file
either: 0 citations name a retired `.inl`/`.tpp`.

It is opt-in per project (`PROSE_ON`), because a guard switched on over a red tree gets
disabled within a week.  `--prose` / `--no-prose` override for a measuring run.
"""
import os, re, sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)                       # project root

# --------------------------------------------------------------------------------------------
# WHICH of the four projects is this checkout?  Answered from the tree's CONTENT, never from the
# directory it happens to sit in.  Identical in `llm-guard.py` and `gen-llms-txt.py`, so the
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
#                    skipped and its anti-vacuous floor never ran at all.  THIS script was the
#                    dangerous one: the other two go red, this one printed its green line with
#                    the "; N prose citations checked" suffix simply absent.
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
if PREFIX is None:
    # HARD STOP, not a fallback.  Every per-project table here (PROSE_FLOOR, PROSE_ON) is keyed
    # on PREFIX, so an unidentified project does not get a weaker check — it gets NO prose check
    # and NO floor, while still printing this script's green line.  That is the exact shape this
    # battery exists to refuse, and it is what shipped.
    print("  cite-check: cannot identify which project %s is — it has neither a "
          "cmake/qbConfig.cmake setting QB_FRAMEWORK_NAME \"qb\" nor a CMakeLists.txt with "
          "project(qbm-<mod> …) naming one of %s." % (ROOT, list(PROJECT_PREFIXES)))
    print("  Refusing to run: the prose form and its anti-vacuous floor are both keyed on the "
          "project, so an unidentified one would pass over unchecked text.")
    sys.exit(2)

# Three citation forms: HTML comment (prose), // line comment (in code fences),
# and (src: ...) inline parenthetical.
FORMS = [re.compile(r"<!--\s*src:\s*(.*?)\s*-->"),
         re.compile(r"//\s*src:\s*([^\n]*?)\s*$"),
         re.compile(r"\(\s*src:\s*([^)]*?)\s*\)")]
PATHLINE = re.compile(r"^([\w./+\-]+\.\w+|[\w./+\-]+/[\w./+\-]+)(?::([\d,\-]+))?$")
RANGEONLY = re.compile(r"^[\d]+(?:-[\d]+)?$")
OTHER_PROJ = re.compile(r"^(qb|qbm/http|qbm/pgsql|qbm/redis)/")

# --------------------------------------------------------------------------- prose form
# The extension allowlist is the whole discriminator between a citation and a host:port.
# It covers what the books actually cite: sources, the build files (`qbConfig.cmake:100`,
# `CMakeLists.txt:126-195`, `CMakePresets.json:9-25` — 136 citations, blind the same way),
# `doc-lint.sh:44-51`, and cross-page `.md`.  `example.org:4433`, `www.example.com:8080`,
# `db.internal:5432` are excluded by construction rather than by a denylist, and the
# confidence check below is what stops the allowlist from silently going stale.
EXT = r"(?:h|hpp|cpp|cc|inl|tpp|cmake|txt|json|md|sh)"
# Backticks are NOT required: 128 of these citations are bare in the sentence
# (`... (geo_commands.h:105-109) is the lone exception`), and a backtick-anchored pattern
# reads every one of them as prose.  LINES tolerates a spaced comma list
# (`redis.h:689-690, 847-848`) and a trailing ellipsis (`reply.h:317,339,449,…`); a tighter
# spec matched NOTHING on those lines rather than matching part of them.
PROSE = re.compile(r"(?<![\w./+\-])([A-Za-z0-9_][\w./+\-]*\." + EXT + r")"
                   r":(\d+(?:-\d+)?(?:\s*,\s*(?:\d+(?:-\d+)?|…|\.\.\.))*)")
# A continuation range re-opened on its own, bound to the nearest citation BEFORE it:
#   | Return types | ... | `task.h:361`, `:716` |          (backticked — 50)
#   ... — connection_commands.h:79 / :94                   (slash-joined — 30)
# It must bind by POSITION, not to the last path on the line: `(`Actor.h:1186-1187` and
# `:1098`) ... (`Actor.cpp:260-262`)` bound `:1098` to Actor.cpp (357 lines) and reported a
# BAD RANGE that was the checker's, not the doc's.  CONT_LEAD is what keeps an ordinary
# `**Note**: 3` from being read as a range.
PROSE_CONT = re.compile(r"(?<![\w.\-/:]):(\d+(?:-\d+)?(?:\s*,\s*\d+(?:-\d+)?)*)")
CONT_LEAD = ("`", "/", ",")
# CONFIDENCE CHECK.  A guard is worthless until it has been shown to fail, and a citation
# parser fails SILENTLY: a shape it does not recognise reads as prose and is skipped, so the
# run stays green over unchecked text.  So an extension OUTSIDE the allowlist whose basename
# is a real file in this project is reported as UNPARSED rather than ignored.  This is not
# decoration — it is what found `.sh` and `.md`, the spaced-comma and ellipsis line specs,
# the 128 un-backticked citations and the mis-binding above, each of which had been written
# off as "no such shape exists".  A host name (`example.org`) names no file here and stays
# quiet, so the check needs no denylist to maintain.
UNKNOWN_EXT = re.compile(r"(?<![\w./+\-])([A-Za-z0-9_][\w./+\-]*\.([A-Za-z][A-Za-z0-9]*)):\d")
# The superproject root is not this project.  `resolve_in_project` strips a leading
# `qb-dev/` so that `qb-dev/qb/src/...` resolves, but `qb-dev/CMakeLists.txt:15` names the
# SUPERPROJECT's own CMakeLists.txt — which, stripped, collides with `qb/CMakeLists.txt`
# and would be range-checked against the wrong file entirely (413 lines vs 277: in range, so
# silently green, and line 15 there is `# limitations under the License.`).  A `qb-dev/` path
# whose remainder is not itself inside a project is superproject-owned: another project, SKIP.
SUPER_OWNED = re.compile(r"^(?!qb/|qbm/|examples/|llm/|dev/|cmake/)")

# Anti-vacuous floor, PER PROJECT — four projects are four surfaces, and one shared number
# lets one surface silently absorb another's collapse.  A sweep that parses 0 citations
# because a readme/ directory moved passes every other check in this file while proving
# nothing.  Raise a floor when a book grows; never lower one to make a run pass.
PROSE_FLOOR = {"qb/": 660, "qbm/http/": 20, "qbm/pgsql/": 35, "qbm/redis/": 315}
# Projects where the prose form is ON by default.  A project joins this set only once its
# count is ZERO; enabling it over a red tree just teaches people to pass --no-prose.
PROSE_ON = {"qb/", "qbm/http/", "qbm/pgsql/", "qbm/redis/"}

# A project that IS identified but is missing from either table above loses the prose form and
# its floor silently — the same defect as an unidentifiable project, one layer in.  That is not
# hypothetical: the whole reason this file needed fixing is that `PREFIX in PROSE_ON` returning
# False was a valid Python expression producing a green run.  Asserted, not intended.
_gaps = [p for p in PROJECT_PREFIXES if p not in PROSE_FLOOR or p not in PROSE_ON]
if _gaps:
    print("  cite-check: %s can be identified but is absent from PROSE_FLOOR and/or PROSE_ON, "
          "so the prose form would not run there and no floor would notice" % _gaps)
    sys.exit(2)

_lc = {}
def nlines(p):
    """Line count for a file; -1 for an existing directory (exists, not range-checkable); None if absent."""
    if p not in _lc:
        if os.path.isdir(p):
            _lc[p] = -1
        else:
            try:
                with open(p, "rb") as fh: _lc[p] = sum(1 for _ in fh)
            except OSError: _lc[p] = None
    return _lc[p]

_byname = None
def by_basename(name):
    """Every file in the project with this basename, as absolute paths."""
    global _byname
    if _byname is None:
        _byname = {}
        for dp, dns, fns in os.walk(ROOT):
            dns[:] = [d for d in dns if d not in (".git", "build", "node_modules", ".cache")]
            for f in fns:
                _byname.setdefault(f, []).append(os.path.join(dp, f))
    return _byname.get(os.path.basename(name), [])

def is_project_file(name):
    """Does a file with this basename exist anywhere in the project? (confidence check only)"""
    return bool(by_basename(name))

def resolve_prose(tok):
    """Resolve a PROSE citation to the file(s) it could name.

    Returns (kind, sizes, paths): 'OK' with one line count, 'AMBIG' with several (in range
    for ANY candidate is enough), 'SKIP' (another project's file — not ours), or 'MISS'.

    THE BARE BASENAME IS THE DOMINANT FORM and `resolve_in_project` calls it prose.  That
    inherited rule is right for a `src:` body, where a bare token really can be a hint word,
    and catastrophically wrong here: `resultset.h:104`, `redis.h:935`, `task.h:361`,
    `geo_commands.h:105-109` all carry an unambiguous line number, and 443 of the 1010
    citations (44%) are written that way.  Skipping them made the FIRST version of this
    check report zero violations on all four repos while a replanted wrong-line defect —
    the real one from `qbm/pgsql` `12268dd` — sailed straight through it.  So a token with
    a line spec is resolved by path SUFFIX against the project's own file index, the way
    `llm-guard.py`'s Index does, and only a basename that exists nowhere here is prose.
    """
    ab, n = resolve_in_project(tok)
    if ab not in ("SKIP", None):
        return "OK", [n], [ab]
    p = tok.strip().lstrip("./")
    cands = [c for c in by_basename(p)
             if c.endswith("/" + p) or os.path.basename(c) == p]
    if len(cands) == 1:
        return "OK", [nlines(cands[0])], cands
    if len(cands) > 1:
        return "AMBIG", [nlines(c) for c in cands], cands
    return ("MISS", None, []) if ab is None else ("SKIP", None, [])

def resolve_in_project(tok):
    """Return (abspath, nlines) if tok resolves INSIDE this project, ('SKIP',None)
    if it belongs to another project, (None,None) if in-project but missing."""
    p = tok.strip().lstrip("./")
    if p.startswith("qb-dev/"):
        p = p[len("qb-dev/"):]
        if SUPER_OWNED.match(p):
            return "SKIP", None                          # superproject's own file
    cands = [os.path.join(ROOT, p)]
    if p.startswith(PREFIX):
        cands.append(os.path.join(ROOT, p[len(PREFIX):]))
    for c in cands:
        n = nlines(c)
        if n is not None:
            return c, n
    m = OTHER_PROJ.match(p)
    if m and not p.startswith(PREFIX):
        return "SKIP", None                              # other project's file
    if p.startswith(PREFIX):
        return None, None                                # ours, but gone
    # A module-internal relative path (tests/…, src/…, include/…, etc.) that did not
    # resolve is a stale in-project citation, not prose — flag it.
    if re.match(r"^(tests|src|source|include|readme|cmake)/", p):
        return None, None
    return "SKIP", None                                  # bare/ambiguous -> prose

def bad_ranges(s, total):
    out = []
    for part in s.split(","):
        part = part.strip()
        if not part: continue
        if "-" in part:
            a, _, b = part.partition("-")
            try: a, b = int(a), int(b)
            except ValueError: continue
        else:
            try: a = b = int(part)
            except ValueError: continue
        if total is not None and total >= 0 and (a < 1 or b < a or b > total):
            out.append(f"{part}(file={total})")
    return out

_txt = {}
def flines(p):
    if p not in _txt:
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                _txt[p] = fh.read().split("\n")
        except OSError: _txt[p] = None
    return _txt[p]

def blank_targets(spec, paths):
    """Single-line components of `spec` that land on a BLANK source line.

    A range legitimately spans blank lines; a citation that names ONE line and lands on
    nothing is a drift, and it is IN RANGE, so it survives every check this script had.
    That is the exact shape of the defect this whole extension was written for --
    `qbm/pgsql/readme/results.md` cited `transaction_coro.inl:96,128,211` where line 96 was
    blank and 211 a bare `}` (fixed in qbm/pgsql `12268dd`).  26 more were live when the
    rule was added, in all three books that have one.  Every component of a comma list is
    checked, not just the first: `ActorId.h:403,444` had BOTH wrong and a first-component-
    only sweep reported it as one finding, hiding the second.
    """
    if len(paths) != 1:
        return []                                        # ambiguous: cannot say which file
    src = flines(paths[0])
    if src is None:
        return []
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part.isdigit():
            continue
        k = int(part)
        if 1 <= k <= len(src) and not src[k - 1].strip():
            out.append(part)
    return out

def bad_spec(spec, sizes):
    """Out-of-range parts of `spec`, given every candidate file it could name.  A citation
    that is in range for ANY candidate passes: 19 redis citations name a basename that two
    files share, and demanding all of them agree would invent failures."""
    if not sizes:
        return []
    per = [bad_ranges(spec, n) for n in sizes]
    return [] if any(not b for b in per) else min(per, key=len)

def doc_files():
    for f in ("README.md",):
        if os.path.isfile(os.path.join(ROOT, f)): yield os.path.join(ROOT, f)
    rd = os.path.join(ROOT, "readme")
    for dp, _, fns in os.walk(rd):
        for fn in sorted(fns):
            if fn.endswith(".md"): yield os.path.join(dp, fn)

do_prose = PREFIX in PROSE_ON
if "--prose" in sys.argv: do_prose = True
if "--no-prose" in sys.argv: do_prose = False

problems = []
prose_seen = 0
for md in doc_files():
    rel = os.path.relpath(md, ROOT)
    for ln, line in enumerate(open(md, encoding="utf-8", errors="replace"), 1):
        for rx in FORMS:
          for m in rx.finditer(line):
            body = m.group(1).strip()
            if not body or "src:" in body: continue
            nohint = re.sub(r"\([^)]*\)", "", body)
            for chunk in re.split(r"[;]", nohint):
                cur = None
                for tok in chunk.strip().rstrip(".,").split(","):
                    tok = tok.strip()
                    if not tok: continue
                    pm = PATHLINE.match(tok)
                    if pm:
                        ab, n = resolve_in_project(pm.group(1))
                        cur = (ab, n)
                        if ab is None:
                            problems.append((rel, ln, f"MISSING {pm.group(1)}"))
                        elif ab != "SKIP" and pm.group(2):
                            for b in bad_ranges(pm.group(2), n):
                                problems.append((rel, ln, f"BAD RANGE {pm.group(1)}:{b}"))
                    elif RANGEONLY.match(tok) and cur and cur[0] not in (None, "SKIP"):
                        for b in bad_ranges(tok, cur[1]):
                            problems.append((rel, ln, f"BAD RANGE (cont) :{b}"))

        # ---- the prose form ------------------------------------------------------
        if not do_prose:
            continue
        # Blank out the three `src:` bodies first: they are already checked above, and a
        # path inside one is the same text as a prose citation.
        bare = line
        for rx in FORMS:
            bare = rx.sub(lambda m: " " * len(m.group(0)), bare)

        hits = []                                        # (start, end, path, kind, sizes)
        for m in PROSE.finditer(bare):
            prose_seen += 1
            path, spec = m.group(1), m.group(2).replace("…", "").replace("...", "")
            kind, sizes, paths = resolve_prose(path)
            hits.append((m.start(), m.end(), path, kind, sizes, paths))
            if kind == "MISS":
                problems.append((rel, ln, f"MISSING {path} (prose)"))
            elif kind != "SKIP":
                for b in bad_spec(spec, sizes):
                    problems.append((rel, ln, f"BAD RANGE {path}:{b} (prose)"))
                for b in blank_targets(spec, paths):
                    problems.append((rel, ln, f"BLANK LINE {path}:{b} — the cited line is "
                                              f"empty, so the citation has drifted"))
        for m in PROSE_CONT.finditer(bare):
            if any(s <= m.start() < e for s, e, *_ in hits):
                continue                                 # inside a path we already read
            lead = bare[:m.start()].rstrip()
            if not lead.endswith(CONT_LEAD):
                continue                                 # ordinary prose colon, not a range
            prior = [h for h in hits if h[1] <= m.start()]
            if not prior:
                continue                                 # nothing to bind to
            _, _, path, kind, sizes, paths = prior[-1]   # nearest PRECEDING citation
            prose_seen += 1
            if kind in ("MISS", "SKIP"):
                continue
            for b in bad_spec(m.group(1), sizes):
                problems.append((rel, ln, f"BAD RANGE {path}:{b} (prose cont)"))
            for b in blank_targets(m.group(1), paths):
                problems.append((rel, ln, f"BLANK LINE {path}:{b} (cont) — the cited line "
                                          f"is empty, so the citation has drifted"))
        for m in UNKNOWN_EXT.finditer(bare):
            if m.group(2) in ("h", "hpp", "cpp", "cc", "inl", "tpp",
                              "cmake", "txt", "json", "md", "sh"):
                continue
            if is_project_file(m.group(1)):
                problems.append((rel, ln, f"UNPARSED citation `{m.group(1)}` — extension "
                                          f"`.{m.group(2)}` is outside EXT, so it goes "
                                          f"unchecked; add it or the guard is blind here"))

if do_prose:
    floor = PROSE_FLOOR.get(PREFIX, 1)
    if prose_seen < floor:
        problems.append(("<prose>", 0, f"ANTI-VACUOUS FLOOR: parsed {prose_seen} prose "
                                       f"citation(s), expected >= {floor} for {PREFIX} — "
                                       f"the surface collapsed, the guard did not pass"))

if problems:
    for p in problems:
        print(f"  {p[0]}:{p[1]}  {p[2]}")
    print(f"cite-check: FAILED ({len(problems)} stale citation(s))")
    sys.exit(1)
print("  all src: citations resolve, line ranges in bounds"
      + (f"; {prose_seen} prose citations checked" if do_prose else ""))
sys.exit(0)
