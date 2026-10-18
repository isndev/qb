#!/usr/bin/env python3
"""check-awaiter-copy.py — an awaiter whose destructor undoes something cannot be copied.

WHY THIS EXISTS
---------------
An awaiter that registers something for its coroutine -- a queue node, a parked handle, an on_cancel
callback, a timer, a slot in a shared state -- undoes it in its destructor. If such an awaiter can be
copied, the copy shares the registration (or the shared state that names it), and whichever object is
destroyed first undoes it for both: the live operation is retracted, its coroutine never resumes, or a
permit, a lock or a value is given back twice. A "move" is no protection: the user-declared destructor
suppresses the implicit move, so the move IS the copy. Nothing fails until a copy is made and destroyed
first -- not the compiler, and not a test that awaits the factory's prvalue as every caller does.

Huly QB-302 found the shape in the connect / select / when_all / when_any awaiters and deleted their copy
and move; QB-1000 found it in twenty-four more (channel, sync, cancellation, scope, shared_task, generator,
quorum) and in qbm-redis's connect awaiter, which is the rule this guard holds from now on.

WHAT IT CHECKS
--------------
Every class or struct that declares `await_suspend(` at its own scope and declares a destructor there:

  C1  it must be non-copyable by construction, one of:
      - `Name(const Name &) = delete;` (or `Name const &`) at its own scope -- the QB-302 shape, with the
        move and both assignments deleted beside it;
      - or a direct base that this run proves non-copyable the same way (`awaiter_base`, which deletes all
        four), while the class declares no copy constructor of its own that would bypass the base.

The class's own scope is its body with every nested brace pair blanked: a function body or a nested class
deleting its copy never protects the class around it.

ESCAPE HATCHES, BOTH NAMED, BOTH CHECKED
----------------------------------------
- `// awaiter-copy: <reason>` on the class head's line or one of the two lines above it: an awaiter whose
  copy is deliberate and safe says why. The reason is mandatory.
- `--known NAME@PATH-SUFFIX`: a reviewed exception kept OUTSIDE the source, for an awaiter in a tree the
  caller may not edit. An entry that names no unprotected class (fixed, renamed or moved) is itself a
  finding (C2): an allowlist that tolerates a dead entry silently widens what the guard ignores.

WHAT IT DELIBERATELY DOES NOT CATCH
-----------------------------------
It is textual. An awaiter produced by a macro, one that inherits `await_suspend` without declaring it, or
one made non-copyable only through a member (a `std::unique_ptr`, a move-only `Reply<T>`) is outside what
it proves: the first is invisible, the second is not an awaiter to it, the third must still delete its
copy explicitly or be named. An awaiter without a destructor is out of scope -- there is nothing to undo
twice. The compiled proof is each owner test's `static_assert` on the four traits.

ANTI-VACUOUS FLOORS
-------------------
Each subject root is `NAME:MIN_FILES:MIN_AWAITERS` -- files visited and awaiter classes found, per root,
never shared, so one tree collapsing cannot hide behind another growing.

USAGE
-----
    ./scripts/check-awaiter-copy.py                                  # qb alone, from the qb root
    ./scripts/check-awaiter-copy.py qb/src:160:54 qbm/http/src:105:1 qbm/pgsql/src:37:4 qbm/redis/src:29:3 \
        --known redis_awaiter@qbm/redis/redis.h

Exit status: 0 clean, 1 findings, 2 usage/IO error or a vacuous run.
"""

from __future__ import annotations

import importlib.util
import os
import re
import sys

sys.dont_write_bytecode = True  # importing the sibling must not leave a __pycache__ beside it
_spec = importlib.util.spec_from_file_location(
    "_awaiter_tracking", os.path.join(os.path.dirname(os.path.abspath(__file__)), "check-awaiter-tracking.py"))
_trk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_trk)
strip_comments, brace_spans, head_of, walk = _trk.strip_comments, _trk.brace_spans, _trk.head_of, _trk.walk

CLASS_HEAD_RE = re.compile(r"\b(class|struct)\s+(?:alignas\s*\([^)]*\)\s*)?(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)")
ANNOTATION_RE = re.compile(r"//\s*awaiter-copy:(.*)$")  # one line: the reason is the rest of THAT line
BASE_NAME_RE = re.compile(r"(?:public|protected|private|virtual|\s)*\s*(?:::)?(?:[A-Za-z_]\w*\s*::\s*)*([A-Za-z_]\w*)")


def blank_template_params(head: str) -> str:
    """Blank every `template <...>` parameter list, so `template <class E> struct x` names x, not E."""
    while True:
        m = re.search(r"\btemplate\s*<", head)
        if not m:
            return head
        depth, k = 0, m.end() - 1
        while k < len(head):
            depth += {"<": 1, ">": -1}.get(head[k], 0)
            if depth == 0:
                break
            k += 1
        head = head[: m.start()] + " " * (k + 1 - m.start()) + head[k + 1 :]


def class_of(head: str):
    """(name, bases) when the brace's head introduces a class or struct, else None."""
    h = blank_template_params(head)
    if re.search(r"\benum\s+(?:class|struct)\b", h):
        return None
    m = CLASS_HEAD_RE.search(h)
    if not m:
        return None
    tail = h[m.end() :]
    before_bases, _, bases = tail.partition(":")
    if "(" in before_bases or "=" in tail or ";" in tail:
        return None  # a function returning a struct, an alias, an elaborated declaration
    names = []
    depth, cur = 0, ""
    for ch in bases:  # split on top-level commas: `base<a, b>` is one base
        depth += {"<": 1, ">": -1}.get(ch, 0)
        if ch == "," and depth == 0:
            names.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        names.append(cur)
    base_names = []
    for b in names:
        b = re.sub(r"<.*", "", b.strip())
        bm = BASE_NAME_RE.fullmatch(b.strip()) if b.strip() else None
        if bm:
            base_names.append(bm.group(1))
    return m.group(2), base_names


def own_scope(src: str, spans, o: int, c: int) -> str:
    """The class body with every nested brace pair blanked: what is declared at the class's own scope."""
    body = list(src[o + 1 : c])
    for so, sc in spans:
        if o < so and sc < c:
            for k in range(so - o, sc - o - 1):
                if body[k] != "\n":
                    body[k] = " "
    return "".join(body)


def copy_ctor_re(name: str) -> re.Pattern:
    n = re.escape(name)
    return re.compile(n + r"\s*\(\s*(?:const\s+" + n + r"|" + n + r"\s+const)\s*&\s*\w*\s*\)(\s*=\s*delete\b)?")


def scan_file(path: str):
    """Every class of the file: (path, line, name, bases, is_awaiter, has_dtor, deletes_copy, own_copy, note)."""
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    src = strip_comments(raw)
    raw_lines = raw.split("\n")
    spans = brace_spans(src)
    rows = []
    for o, c in spans:
        cls = class_of(head_of(src, o))
        if not cls:
            continue
        name, bases = cls
        scope = own_scope(src, spans, o, c)
        copies = list(copy_ctor_re(name).finditer(scope))
        deletes_copy = any(m.group(1) for m in copies)
        own_copy = any(not m.group(1) for m in copies)  # a user copy constructor that is not deleted
        is_awaiter = bool(re.search(r"\bawait_suspend\s*\(", scope))
        has_dtor = bool(re.search(r"~\s*" + re.escape(name) + r"\s*\(", scope))
        head = head_of(src, o)
        line = src.count("\n", 0, o - len(head) + (len(head) - len(head.lstrip()))) + 1  # the head's first token
        # Line by line, never joined: a joined window let an empty `// awaiter-copy:` borrow the next line's
        # code (`struct X {`) as its reason -- the control "an annotation without a reason" caught that.
        note = next((m for m in (ANNOTATION_RE.search(l) for l in raw_lines[max(0, line - 3) : line]) if m), None)
        rows.append((path, line, name, bases, is_awaiter, has_dtor, deletes_copy, own_copy, note))
    return rows


def default_args() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Measured at 3.3.0 on qb alone (Huly QB-1000): 191 files, 54 awaiter classes, 46 of them with a destructor --
    # 42 delete their copy, 4 through awaiter_base.
    return [f"{os.path.join(qb_root, 'src')}:160:54"]


def main() -> int:
    argv = sys.argv[1:] or default_args()
    specs, known = [], []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--known":
            if i + 1 >= len(argv) or "@" not in argv[i + 1]:
                print("usage error: --known takes NAME@PATH-SUFFIX", file=sys.stderr)
                return 2
            n, p = argv[i + 1].split("@", 1)
            known.append([n, p.replace("\\", "/"), 0])
            i += 2
            continue
        if a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        parts = a.rsplit(":", 2)
        if len(parts) != 3 or not parts[1].isdigit() or not parts[2].isdigit():
            print(f"usage error: root spec '{a}' must be NAME:MIN_FILES:MIN_AWAITERS", file=sys.stderr)
            return 2
        specs.append((parts[0], int(parts[1]), int(parts[2])))
        i += 1
    if not specs:
        print("usage error: no root", file=sys.stderr)
        return 2
    for root, _, _ in specs:
        if not os.path.isdir(root):
            print(f"::error::no such root: {root}", file=sys.stderr)
            return 2

    per_root, everything = [], []
    for root, min_files, min_awaiters in specs:
        visited, rows = 0, []
        for path in walk(root):
            visited += 1
            r = scan_file(path)
            if r is None:
                print(f"::error::{path}: unreadable", file=sys.stderr)
                return 2
            rows.extend(r)
        per_root.append((root, min_files, min_awaiters, visited, rows))
        everything.extend(rows)

    # A base is proven non-copyable when every class of that name in this run deletes its copy, or is
    # itself proven so through its own bases -- and at least one exists. Iterate to a fixed point.
    by_name: dict[str, list] = {}
    for row in everything:
        by_name.setdefault(row[2], []).append(row)
    proven: set[str] = {n for n, rs in by_name.items() if all(r[6] for r in rs)}
    changed = True
    while changed:
        changed = False
        for n, rs in by_name.items():
            if n in proven:
                continue
            if all(r[6] or (not r[7] and any(b in proven for b in r[3])) for r in rs):
                proven.add(n)
                changed = True

    findings: list[str] = []
    totals = [0, 0, 0, 0, 0, 0]  # files, awaiters, with dtor, deleted, by base, annotated
    for root, min_files, min_awaiters, visited, rows in per_root:
        awaiters = [r for r in rows if r[4]]
        with_dtor = [r for r in awaiters if r[5]]
        deleted = by_base = annotated = 0
        for path, line, name, bases, _, _, deletes_copy, own_copy, note in with_dtor:
            if deletes_copy:
                deleted += 1
                continue
            base = next((b for b in bases if b in proven), None)
            if base and not own_copy:
                by_base += 1
                continue
            if note and note.group(1) and note.group(1).strip():
                annotated += 1
                continue
            rel = path.replace("\\", "/")
            hit = next((k for k in known if k[0] == name and rel.endswith(k[1])), None)
            if hit:
                hit[2] += 1
                annotated += 1
                continue
            why = ("its own copy constructor is not deleted and bypasses its base" if own_copy and base else
                   "an `awaiter-copy:` annotation needs a reason" if note else
                   "nothing deletes its copy")
            findings.append(
                f"{path}:{line}: [C1] {name} declares await_suspend and a destructor, but {why}: a copy shares "
                f"what the destructor undoes, and the one destroyed first undoes it for the live awaiter (a "
                f"retracted wait that never resumes, a grant returned twice). Delete the four -- "
                f"`{name}(const {name} &) = delete;`, the move and both assignments (Huly QB-302, QB-1000) -- "
                f"or annotate `// awaiter-copy: <reason>` if the copy is deliberate and safe.")
        print(f"  {root}: {visited} sources+headers visited (floor {min_files}), {len(awaiters)} awaiter classes "
              f"(floor {min_awaiters}), {len(with_dtor)} with a destructor: {deleted} delete their copy, "
              f"{by_base} through a proven base, {annotated} named")
        if visited < min_files:
            print(f"::error::{root} visited {visited} sources, below its floor of {min_files}", file=sys.stderr)
            return 2
        if len(awaiters) < min_awaiters:
            print(f"::error::{root}: {len(awaiters)} awaiter classes found, below its floor of {min_awaiters}; the "
                  "class scanner is not working or the tree lost its awaiters -- refusing a vacuous green.",
                  file=sys.stderr)
            return 2
        for k, v in zip(range(6), (visited, len(awaiters), len(with_dtor), deleted, by_base, annotated)):
            totals[k] += v

    for name, suffix, used in known:
        if not used:
            findings.append(f"--known {name}@{suffix}: [C2] names no unprotected awaiter of this run -- it was fixed, "
                            f"renamed or moved; delete the entry, or it silently widens what the guard ignores.")

    if findings:
        for f in findings:
            print(f"::error::{f}", file=sys.stderr)
        print(f"\n{len(findings)} finding(s).", file=sys.stderr)
        return 1
    print(f"OK: {totals[0]} sources+headers, {totals[1]} awaiter classes, {totals[2]} with a destructor -- "
          f"{totals[3]} delete their copy, {totals[4]} through a proven base, {totals[5]} named -- 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
