#!/usr/bin/env python3
"""cite-check.py — citation-integrity guard (installed at <project>/scripts/).

Validates every `<!-- src: PATH:LINES -->` comment in this project's Markdown:
  * the cited file exists, and
  * every cited line range falls within the file's current length.

Resolves PATHs that belong to THIS project (module-relative like `tests/...`,
`commands/foo.h`, or the repo-root form `qbm/redis/...`). Citations that point at
a *different* project are skipped — each project's own guard checks its files, and
in CI a module is checked out in isolation without its siblings.

Exit 0 = clean, 1 = at least one missing file or out-of-bounds range.
Run from the project root:  python3 scripts/cite-check.py
"""
import os, re, sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)                       # project root
# Project prefix in repo-root-form citations (…/qbm/http -> "qbm/http/").
parts = ROOT.replace(os.sep, "/").split("/")
if parts[-2:-1] == ["qbm"]:
    PREFIX = "/".join(parts[-2:]) + "/"                  # qbm/http/
else:
    PREFIX = parts[-1] + "/"                             # qb/

# Three citation forms: HTML comment (prose), // line comment (in code fences),
# and (src: ...) inline parenthetical.
FORMS = [re.compile(r"<!--\s*src:\s*(.*?)\s*-->"),
         re.compile(r"//\s*src:\s*([^\n]*?)\s*$"),
         re.compile(r"\(\s*src:\s*([^)]*?)\s*\)")]
PATHLINE = re.compile(r"^([\w./+\-]+\.\w+|[\w./+\-]+/[\w./+\-]+)(?::([\d,\-]+))?$")
RANGEONLY = re.compile(r"^[\d]+(?:-[\d]+)?$")
OTHER_PROJ = re.compile(r"^(qb|qbm/http|qbm/pgsql|qbm/redis)/")

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

def resolve_in_project(tok):
    """Return (abspath, nlines) if tok resolves INSIDE this project, ('SKIP',None)
    if it belongs to another project, (None,None) if in-project but missing."""
    p = tok.strip().lstrip("./")
    if p.startswith("qb-dev/"): p = p[len("qb-dev/"):]
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

def doc_files():
    for f in ("README.md",):
        if os.path.isfile(os.path.join(ROOT, f)): yield os.path.join(ROOT, f)
    rd = os.path.join(ROOT, "readme")
    for dp, _, fns in os.walk(rd):
        for fn in fns:
            if fn.endswith(".md"): yield os.path.join(dp, fn)

problems = []
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

if problems:
    for p in problems:
        print(f"  {p[0]}:{p[1]}  {p[2]}")
    print(f"cite-check: FAILED ({len(problems)} stale citation(s))")
    sys.exit(1)
print("  all src: citations resolve, line ranges in bounds")
sys.exit(0)
