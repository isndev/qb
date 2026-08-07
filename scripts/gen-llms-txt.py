#!/usr/bin/env python3
"""gen-llms-txt.py — generate (and gate) this repo's `llms.txt` + `llms-full.txt`.

Installed byte-identical at `<project>/scripts/gen-llms-txt.py` in qb, qbm-http, qbm-pgsql
and qbm-redis, exactly as `cite-check.py` and `llm-guard.py` are, and driven from the same
place: `scripts/doc-lint.sh`, which each repo's `doc-lint.yml` runs.  `dev/agent/verify.sh`
asserts the four copies are byte-identical.

WHAT IT PRODUCES
----------------
`/llms.txt`  — the llmstxt.org index.  The format that specification fixes is, in order:
an H1 (the only required section), a blockquote summary, zero or more non-heading prose
sections, then zero or more H2-delimited file lists whose entries are
`- [name](url): notes`.  A section literally titled `## Optional` has defined meaning:
"the URLs provided there can be skipped if a shorter context is needed".  Every one of
those constraints is asserted by `check()` below rather than merely intended.
(Spec: https://llmstxt.org/ — read 2026-08-07.)

`/llms-full.txt` — the two `llm/` agent docs concatenated.  This name is a de-facto
convention, NOT part of the llmstxt.org specification, which instead describes
`llms-ctx.txt` / `llms-ctx-full.txt` built by the `llms_txt2ctx` tool.  It is used here
because it is what current consumers look for; the header of the generated file says so,
so nobody later mistakes it for a spec artefact.

WHY GENERATED, AND WHY THE CHECK IS THE POINT
---------------------------------------------
`dev/analysis/DISTRIBUTION-3.0.md` §7.5 states the risk in one line: every publication
channel is "a copy of a guarded original living where the guard cannot see it", and this
project's `llm/` surface measured 10.1% wrong the last time anybody counted.  So nothing
here is hand-written.  `llms-full.txt` is a byte concatenation of the `llm/` docs, and
every prose line of `llms.txt` is lifted verbatim out of the `<!-- llms-txt:lead -->`
region of `llm/*.llm.md`.  `--check` regenerates both in memory and compares byte for
byte, so editing `llm/` without regenerating is a FAILURE, not a silent divergence.

The link lists are enumerated from the files that actually exist in the checkout, and
every generated URL is resolved back to a path in this repo and asserted to exist.  That
resolution is deliberately LOCAL: the published URLs name `main`, which will not carry
3.0.0 until the release merge, so a network link check would either 404 or validate the
previous release's content.  A checker that cannot be right must not be run.

ANTI-VACUOUS-PASS
-----------------
`FLOORS` is per project: minimum link count, minimum `llms-full.txt` bytes, minimum lead
bytes.  A generator that emitted an empty index would otherwise "match" an empty
committed file and pass.  Raise a floor when a doc grows; lowering one is a deliberate
act that belongs in a commit message.

Usage:  python3 scripts/gen-llms-txt.py --check    # gate: regenerate and diff (CI)
        python3 scripts/gen-llms-txt.py --write    # rewrite both files
        python3 scripts/gen-llms-txt.py --stats
Exit 0 = clean, 1 = drift / floor violation / malformed output, 2 = cannot determine the
project or its version.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)

# Project identity, computed exactly as llm-guard.py and cite-check.py compute it, so the
# three scripts cannot disagree about what "this project" is.
_parts = ROOT.replace(os.sep, "/").split("/")
PREFIX = ("/".join(_parts[-2:]) + "/") if _parts[-2:-1] == ["qbm"] else (_parts[-1] + "/")
PROJECT = PREFIX.rstrip("/").replace("/", "-")           # qb | qbm-http | qbm-pgsql | qbm-redis

# GitHub repository each project publishes from.  These are the five public repos named in
# AGENTS.md; the private superproject `isndev/qb-dev` deliberately has no entry, because
# nothing it holds is fetchable by an agent.
REPO = {
    "qb/": "isndev/qb",
    "qbm/http/": "isndev/qbm-http",
    "qbm/pgsql/": "isndev/qbm-pgsql",
    "qbm/redis/": "isndev/qbm-redis",
}

# Anti-vacuous floors, per project: (min links, min llms-full bytes, min lead bytes).
# Measured counts are in the comment beside each; the floors sit just under.
FLOORS = {
    #              links  full-bytes  lead-bytes
    "qb/":          (60,     125000,       2000),   # 67 / 135_990 / 2627
    "qbm/http/":    (26,     110000,       2000),   # 29 / 121_506 / 2357
    "qbm/pgsql/":   (13,      62000,       2000),   # 15 /  71_140 / 2507
    "qbm/redis/":   (30,      80000,       2000),   # 33 /  88_711 / 2748
}

# The published ref.  Every AI-docs channel measured in DISTRIBUTION-3.0.md §5.1 (GitMCP,
# DeepWiki, Context7) reads the repository's DEFAULT branch, and all five public repos have
# `default_branch = main`.  Pinning the links to `main` is therefore what makes them
# resolve for a consumer; it also means the links are one release behind until the 3.0.0
# merge, which is why check() validates them against the local tree and not the network.
REF = "main"

LEAD_OPEN = "<!-- llms-txt:lead -->"
LEAD_CLOSE = "<!-- /llms-txt:lead -->"

GENERATED_BY = "scripts/gen-llms-txt.py"


# --------------------------------------------------------------------------- helpers

def rd(rel: str) -> str:
    with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
        return fh.read()


def exists(rel: str) -> bool:
    return os.path.isfile(os.path.join(ROOT, rel))


def url(rel: str) -> str:
    return "https://raw.githubusercontent.com/%s/%s/%s" % (REPO[PREFIX], REF, rel)


def llm_docs() -> list[str]:
    d = os.path.join(ROOT, "llm")
    if not os.path.isdir(d):
        return []
    # Concepts before API reference: an agent that reads only the first link should get the
    # mental model, not an alphabetical accident (`qb.llm.api.md` sorts before `qb.llm.md`).
    return sorted(("llm/" + f for f in os.listdir(d) if f.endswith(".md")),
                  key=lambda f: (f.endswith(".llm.api.md"), f))


def title_of(rel: str) -> str:
    """The document's own H1, stripped of markdown emphasis; the path if it has none."""
    for line in rd(rel).split("\n"):
        m = re.match(r"^#\s+(.*?)\s*$", line)
        if m:
            return re.sub(r"[`*]", "", m.group(1))
    return rel


def note_of(rel: str) -> str:
    """First prose sentence after the H1 — the link's `: notes` half.

    Skips blockquotes, badges, HTML comments, tables and fenced blocks so the note is a
    sentence rather than a shields.io URL."""
    body = re.sub(r"```.*?```", "", rd(rel), flags=re.S)
    lines, seen_h1, para = body.split("\n"), False, []
    for line in lines:
        s = line.strip()
        if not seen_h1:
            seen_h1 = bool(re.match(r"^#\s+", s))
            continue
        if para and not s:
            break                                          # paragraph ended
        if not s or s.startswith(("#", ">", "|", "<!--", "<p", "[!", "!", "---", "*   ", "- ")):
            if para:
                break
            continue
        para.append(re.sub(r"<!--.*?-->", "", s).strip())
    # Join the WHOLE paragraph before looking for a sentence end.  Reading only the first
    # physical line truncated every hard-wrapped lead ("... libraries of the"), which is how
    # a generated index ends up quoting half-sentences at an agent.
    s = " ".join(x for x in para if x)
    if not s:
        return ""
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)         # [text](url) -> text
    s = re.sub(r"[`*]", "", s)
    s = re.sub(r"\s+", " ", s).strip()
    m = re.match(r"^(.{20,240}?[.!?])(?:\s|$)", s)
    return (m.group(1) if m else s)[:240]


def lead() -> str:
    """The verbatim `<!-- llms-txt:lead -->` region of this project's `.llm.md`.

    That region holds the blockquote the spec requires plus the rules a model must not get
    wrong.  It lives inside the agent doc, visible and guarded there by llm-guard.py, so
    llms.txt has no prose of its own to rot independently."""
    src = [d for d in llm_docs() if d.endswith(".llm.md")]
    if not src:
        die("no llm/*.llm.md in %s — nothing to generate a lead from" % PROJECT)
    text = rd(src[0])
    i, j = text.find(LEAD_OPEN), text.find(LEAD_CLOSE)
    if i < 0 or j < 0 or j < i:
        die("%s carries no %s … %s region; llms.txt cannot be generated without one"
            % (src[0], LEAD_OPEN, LEAD_CLOSE))
    return text[i + len(LEAD_OPEN):j].strip("\n")


def die(msg: str) -> None:
    print("  FAIL [%s] %s" % (PROJECT, msg))
    sys.exit(2)


# --------------------------------------------------------------------------- generation

def sections() -> list[tuple[str, list[str]]]:
    """(H2 title, [repo-relative paths]) — enumerated from the checkout, never hand-listed."""
    agent = llm_docs()
    project = [f for f in ("README.md", "INSTALL.md", "VERSIONING.md") if exists(f)]
    book = sorted(
        os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, "/")
        for dp, _, fs in os.walk(os.path.join(ROOT, "readme"))
        for f in fs if f.endswith(".md")
    )
    optional = book + [f for f in ("CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md",
                                   "SUPPORT.md", "CODE_OF_CONDUCT.md", "ROADMAP.md")
                       if exists(f)]
    return [("Agent reference", agent), ("Project", project), ("Optional", optional)]


def gen_llms_txt() -> str:
    out = ["# %s" % title_of("README.md"), ""]
    out += [lead(), ""]
    for name, files in sections():
        if not files:
            continue
        out.append("## %s" % name)
        out.append("")
        if name == "Agent reference":
            out.append("- [%s](%s): both agent docs above, concatenated — one fetch, "
                       "the whole machine-facing surface of this repo."
                       % ("llms-full.txt", url("llms-full.txt")))
        for rel in files:
            note = note_of(rel)
            out.append("- [%s](%s)%s" % (title_of(rel), url(rel), ": " + note if note else ""))
        out.append("")
    out.append("<!-- generated by %s from llm/ + the files in this repo; "
               "run `python3 %s --write` after editing llm/ -->" % (GENERATED_BY, GENERATED_BY))
    return "\n".join(out).rstrip("\n") + "\n"


def gen_llms_full() -> str:
    docs = llm_docs()
    head = [
        "# %s — full agent documentation" % title_of("README.md"),
        "",
        "> Concatenation of this repository's `llm/` agent docs, in one file, for an agent",
        "> that wants the whole surface in a single fetch. The index is `llms.txt`.",
        "",
        "Generated by `%s`; do not edit. `llms-full.txt` is a de-facto convention, not part"
        % GENERATED_BY,
        "of the llmstxt.org specification (which defines `llms.txt`, and describes",
        "`llms-ctx.txt` / `llms-ctx-full.txt` as tool output).",
        "",
        "Source documents, in order: %s" % ", ".join("`%s`" % d for d in docs),
        "",
        "---",
        "",
    ]
    body = []
    for d in docs:
        body += ["<!-- BEGIN %s -->" % d, "", rd(d).rstrip("\n"), "",
                 "<!-- END %s -->" % d, "", "---", ""]
    return "\n".join(head + body).rstrip("\n") + "\n"


# --------------------------------------------------------------------------- checking

def structure_violations(txt: str) -> list[str]:
    """Assert the llmstxt.org shape on the bytes we are about to publish."""
    bad = []
    lines = txt.split("\n")
    if not lines or not lines[0].startswith("# "):
        bad.append("llms.txt must begin with an H1")
    rest = [l for l in lines[1:] if l.strip()]
    if not rest or not rest[0].startswith("> "):
        bad.append("the H1 must be followed by a blockquote summary")
    h2 = [l for l in lines if l.startswith("## ")]
    if not h2:
        bad.append("no H2 file-list section")
    if "## Optional" not in h2:
        bad.append("no `## Optional` section (the spec gives that title defined meaning)")
    if any(l.startswith("### ") for l in lines):
        bad.append("H3 headings are not part of the format")
    # every H2 section body must be a link list, and prose must precede the first H2
    seen_h2 = False
    for n, l in enumerate(lines, 1):
        if l.startswith("## "):
            seen_h2 = True
            continue
        if seen_h2 and l.strip() and not l.startswith(("- [", "<!--")):
            bad.append("line %d is inside a file list but is not a `- [name](url)` entry: %r"
                       % (n, l[:60]))
    return bad


def links_of(txt: str) -> list[tuple[str, str]]:
    return re.findall(r"^- \[([^\]]+)\]\(([^)]+)\)", txt, re.M)


def check(write: bool, stats: bool) -> int:
    if PREFIX not in REPO or PREFIX not in FLOORS:
        die("this script is installed at %s, whose project prefix %r is not one of %s — "
            "it cannot know what to generate" % (ROOT, PREFIX, sorted(REPO)))
    fail = 0
    want_txt, want_full = gen_llms_txt(), gen_llms_full()
    min_links, min_full, min_lead = FLOORS[PREFIX]

    # The shape is asserted on the generated text AND on the committed file, for the same
    # reason the link rule is: checking only what we just built is close to vacuous, because
    # the builder emits the shape by construction.  What can actually be malformed is the
    # file in the repo, which is the file an agent fetches.
    for label, txt in (("generated", want_txt),
                       ("committed", rd("llms.txt") if exists("llms.txt") else "")):
        if not txt:
            continue
        for v in structure_violations(txt):
            print("  FAIL [format] (%s) %s" % (label, v))
            fail = 1

    links = links_of(want_txt)
    if len(links) < min_links:
        print("  FAIL [vacuous] llms.txt lists %d links, floor is %d" % (len(links), min_links))
        fail = 1
    if len(want_full) < min_full:
        print("  FAIL [vacuous] llms-full.txt is %d bytes, floor is %d"
              % (len(want_full), min_full))
        fail = 1
    if len(lead()) < min_lead:
        print("  FAIL [vacuous] the llms-txt:lead region is %d bytes, floor is %d"
              % (len(lead()), min_lead))
        fail = 1

    # Every published URL must name a file that exists HERE.  A dead link in llms.txt is
    # worse than no llms.txt: it is what an agent fetches instead of asking.
    #
    # Both the generated text AND the committed file are checked.  Checking only the
    # generated one is VACUOUS by construction: it is enumerated from the files that exist,
    # so it can never list a missing one.  The committed file is where a dead link actually
    # lives — delete a `readme/` chapter without regenerating and the published index points
    # at a 404 until someone notices.
    dead = 0
    pre = "https://raw.githubusercontent.com/%s/%s/" % (REPO[PREFIX], REF)
    committed = rd("llms.txt") if exists("llms.txt") else ""
    for name, u in dict.fromkeys(links + links_of(committed)):
        if not u.startswith(pre):
            print("  FAIL [link] %s -> %s does not point into this repo" % (name, u))
            fail = 1
            continue
        rel = u[len(pre):]
        if rel in ("llms.txt", "llms-full.txt"):
            continue
        if not exists(rel):
            print("  FAIL [link] %s -> %s does not exist in the checkout" % (name, rel))
            dead += 1
            fail = 1
    if write:
        for rel, data in (("llms.txt", want_txt), ("llms-full.txt", want_full)):
            with open(os.path.join(ROOT, rel), "w", encoding="utf-8") as fh:
                fh.write(data)
        print("  wrote llms.txt (%d links) and llms-full.txt (%d bytes)"
              % (len(links), len(want_full)))
    else:
        for rel, data in (("llms.txt", want_txt), ("llms-full.txt", want_full)):
            if not exists(rel):
                print("  FAIL [drift] %s is missing — run `python3 %s --write`"
                      % (rel, GENERATED_BY))
                fail = 1
                continue
            have = rd(rel)
            if have != data:
                print("  FAIL [drift] %s is not what %s produces from the current llm/ "
                      "and docs — run `python3 %s --write` and commit the result"
                      % (rel, GENERATED_BY, GENERATED_BY))
                for ln, (a, b) in enumerate(zip(have.split("\n"), data.split("\n")), 1):
                    if a != b:
                        print("      first difference at line %d" % ln)
                        print("        committed: %s" % a[:110])
                        print("        generated: %s" % b[:110])
                        break
                else:
                    print("      committed is %d lines, generated is %d"
                          % (len(have.split("\n")), len(data.split("\n"))))
                fail = 1

    if stats or not fail:
        print("  %s: links=%d (floor %d) llms-full=%dB (floor %d) lead=%dB dead-links=%d"
              % (PROJECT, len(links), min_links, len(want_full), min_full, len(lead()), dead))
    return fail


def main() -> int:
    ap = argparse.ArgumentParser(description="generate %s's llms.txt / llms-full.txt" % PROJECT)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true",
                   help="regenerate in memory and fail on any difference (default)")
    g.add_argument("--write", action="store_true", help="rewrite both files")
    ap.add_argument("--stats", action="store_true", help="print the counts even when clean")
    a = ap.parse_args()
    return check(write=a.write, stats=a.stats)


if __name__ == "__main__":
    sys.exit(main())
