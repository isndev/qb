#!/usr/bin/env python3
"""check-changelog.py -- every sentence of a CHANGELOG's [Unreleased] section is written to its end (installed at
<project>/scripts/, byte-identical in qb and the three qbm repositories; doc-lint.sh runs it on CHANGELOG.md).

An entry is written, rewritten and moved by hand, and nothing read the section whole: qbm-http's [Unreleased] carried,
from 2026-10-29 to 2026-11-02, an entry cut off in mid-sentence -- "a session class that declares an `on()` of its
own (the" -- left beside its rewrite by the edit that should have replaced it, and doc-lint, cite-check, llm-guard and
verify.sh all passed it (Huly QB-1221). The release body is generated from these entries. In [Unreleased]:

  * every paragraph ends its sentence. A paragraph is a run of non-blank lines that a blank line, a heading or a list
    item ends -- an entry or one of its sub-items, a numbered item only where it can open a list (`1.`, `1)`, or a
    list already open), as Markdown reads it -- so an entry cut short and followed at once by the next one is still a
    paragraph that ends unfinished. Past any closing mark (`)`, `]`, `*`, `_`, a backtick, a quote), its last line
    ends on `.`, `!` or `?`; on `:` only where an indented item, a table or a code block follows, which is what a
    `:` introduces (the next entry is none of them); on `;` only where another item of the same list follows. What
    ends otherwise and is no sentence was listed from every section of the six CHANGELOGs of the superproject (1 165
    paragraphs on 2026-11-03, not one other ending): a table row, a link reference definition
    (`[3.0.0]: https://...`), a thematic break (`---`), and a fenced code block, read from its fence to the fence
    that closes it. A paragraph whose last lines are such lines ends on the last line that is not;
  * no headline is cut off -- its brackets and parentheses balance, past its code spans, as all 972 of the corpus do;
  * no two entries open on the same bold headline, or on two that read alike (difflib's ratio NEAR or more): the
    second copy of an entry rewritten beside the first. The corpus' two such pairs are both copies -- the incident
    above (0.95) and one entry listed twice in qbm-http's published [3.3.0] (1.00); the closest two that are not
    read 0.87;
  * there is one [Unreleased] section.

An entry and its headline are read as dev/agent/release-gate.sh's release_notes() reads them to write the release
body -- the section runs from `## [Unreleased]` to the next `## [`, its `### ` subsections split it, an entry opens
with a line `- `, its headline is the `**...**` it opens with -- and `--headlines` lists them, for the superproject's
battery to compare with what release_notes() publishes.

Exit 0 = clean, 1 = at least one finding, 2 = nothing to check: a missing file, or one with no [Unreleased] section
or no paragraph in it. Usage, from the project root:  python3 scripts/check-changelog.py [--headlines] [FILE ...]
(default: ./CHANGELOG.md)
"""
import difflib
import io
import os
import re
import sys

SECTION = re.compile(r"^## \[Unreleased\][^\n]*\n(.*?)(?=^## \[|\Z)", re.S | re.M)
FENCE = re.compile(r"[ \t]*(`{3,}|~{3,})")
HEADING = re.compile(r"[ \t]*#{1,6}[ \t]")
BULLET = re.compile(r"[ \t]*[-*+][ \t]")
ORDERED = re.compile(r"[ \t]*(\d+)[.)][ \t]")
TABLE = re.compile(r"[ \t]*\|")
EXEMPT = re.compile(r"[ \t]*(?:\|.*|\[[^\]]+\]:[ \t]+\S.*|(?:-[ \t]*){3,}|(?:\*[ \t]*){3,}|(?:_[ \t]*){3,})$")
CLOSERS = r"[)\]*_`\"'”’»]*$"
ENDS, INTRODUCES, ENUMERATES = re.compile(r"[.!?]" + CLOSERS), re.compile(r":" + CLOSERS), re.compile(r";" + CLOSERS)
HEADLINE = re.compile(r"- \*\*(.+?)\*\*", re.S)
CODE_SPAN = re.compile(r"(`+)(?:(?!\1).)+\1")
NEAR = 0.9


def unreleased(text):
    """(the first [Unreleased] body, the line its first line is, how many [Unreleased] headings), or None."""
    section = SECTION.search(text)
    if not section:
        return None
    return section.group(1), text.count("\n", 0, section.start(1)) + 1, len(re.findall(r"(?m)^## \[Unreleased\]", text))


def headlines(body, first):
    """(line, headline) of every entry, split and read exactly as release_notes() does: subsections at `### `, entries
    at `- ` (inside a fence or not), a headline the `**...**` an entry opens with, its whitespace runs made one space;
    an entry without one is (line, None)."""
    out, at = [], 0
    for part in re.split(r"(?m)^(?=### )", body):
        skip = len(part.partition("\n")[0]) + 1 if part.startswith("### ") else 0
        lead, *items = re.split(r"(?m)^(?=- )", part[skip:])
        pos = at + skip + len(lead)
        for item in items:
            head = HEADLINE.match(item)
            out.append((first + body.count("\n", 0, pos), " ".join(head.group(1).split()) if head else None))
            pos += len(item)
        at += len(part)
    return out


def balanced(head):
    """Whether a headline's parentheses and brackets close, its code spans set aside, and no backtick is left open."""
    text = CODE_SPAN.sub("", head)
    depth = {"(": 0, "[": 0}
    for c in text:
        if c in "([":
            depth[c] += 1
        elif c in ")]":
            opener = "(" if c == ")" else "["
            if not depth[opener]:
                return False
            depth[opener] -= 1
    return not any(depth.values()) and "`" not in text


def check(path):
    """(findings, paragraphs, entries) of one CHANGELOG's [Unreleased]; findings is None, with why, if it holds none."""
    try:
        text = io.open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    except OSError as error:
        return None, f"{path}: {error.strerror}", 0
    found = unreleased(text)
    if found is None:
        return None, f"{path}: no [Unreleased] section", 0
    body, first, sections = found
    findings, paragraphs = [], 0
    if sections > 1:
        findings.append(f"{path}: {sections} [Unreleased] sections -- only the first is read, by this check as by "
                        f"the release gate")
    block, fence, pending, listing = [], None, None, False  # pending: (line, ':' or ';', its indent, its text)

    def close():
        """End the paragraph in `block`: its last line that is not a table row, a link reference or a break."""
        nonlocal paragraphs, pending
        if block:
            paragraphs += 1
            last = next(((n, l) for n, l in reversed(block) if not EXEMPT.match(l)), None)
            if last:
                number, line = last
                tail = line.rstrip()
                opener = block[0][1]
                if INTRODUCES.search(tail):
                    pending = (number, ":", None, tail)
                elif ENUMERATES.search(tail) and (BULLET.match(opener) or ORDERED.match(opener)):
                    pending = (number, ";", len(opener) - len(opener.lstrip()), tail)
                elif not ENDS.search(tail):
                    findings.append(f"{path}:{number}: [Unreleased] a paragraph ends before its sentence does: "
                                    f"...{tail[-70:]}")
            block.clear()

    def follows(kind, indent=0):
        """What opens next -- its kind, and an item's indent -- settles a paragraph that ended on `:` or `;`."""
        nonlocal pending
        if pending:
            number, mark, own, tail = pending
            if mark == ":" and not (kind in ("table", "fence") or kind == "item" and indent > 0) or \
                    mark == ";" and not (kind == "item" and indent == own):
                what = "no indented item, table or code block" if mark == ":" else "no other item of its list"
                findings.append(f"{path}:{number}: [Unreleased] a paragraph ends on `{mark}` and {what} follows: "
                                f"...{tail[-70:]}")
            pending = None

    for number, line in enumerate(body.split("\n"), first):
        if fence:
            closing = line.strip()
            if closing.startswith(fence) and closing == fence[0] * len(closing):
                fence = None
            continue
        opened = FENCE.match(line)
        if opened:
            close()
            follows("fence")
            fence = opened.group(1)
            continue
        if not line.strip():
            close()
            continue
        heading = HEADING.match(line)
        ordered = ORDERED.match(line)
        item = BULLET.match(line) or ordered and (ordered.group(1) == "1" or listing)
        table = TABLE.match(line) and not (block and TABLE.match(block[-1][1]))
        if heading or item or table:
            close()
        if not block:
            follows("heading" if heading else "item" if item else "table" if TABLE.match(line) else "text",
                    len(line) - len(line.lstrip()))
        if heading:
            listing = False
            continue
        listing = listing or bool(ordered and item)
        block.append((number, line))
    close()
    follows("end")
    if fence:
        findings.append(f"{path}: [Unreleased] a code fence opens and never closes")
    if not paragraphs:
        return None, f"{path}: no paragraph in [Unreleased]", 0
    entries, seen = headlines(body, first), []
    for number, head in entries:
        if head is None:
            continue
        if not balanced(head):
            findings.append(f"{path}:{number}: [Unreleased] a headline cut off -- its brackets do not close: "
                            f"{head[:90]}")
        for other, earlier in seen:
            alike = difflib.SequenceMatcher(None, head, other)
            if head == other or alike.real_quick_ratio() >= NEAR and alike.quick_ratio() >= NEAR and \
                    alike.ratio() >= NEAR:
                how = "the same headline" if head == other else "a headline that reads like the one"
                findings.append(f"{path}:{number}: [Unreleased] {how} at line {earlier}: {head[:90]}")
                break
        seen.append((head, number))
    return findings, paragraphs, len(entries)


def main(argv):
    listing = "--headlines" in argv
    paths = [a for a in argv if a != "--headlines"] or [os.path.join(os.getcwd(), "CHANGELOG.md")]
    bad = paragraphs = entries = 0
    for path in paths:
        if listing:
            try:
                found = unreleased(io.open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n"))
            except OSError as error:
                found, why = None, error.strerror
            if found is None:
                print(f"  check-changelog: {path}: nothing to list")
                return 2
            for number, head in headlines(found[0], found[1]):
                print(f"{path}\t{number}\t{head if head is not None else ''}")
            continue
        findings, seen, items = check(path)
        if findings is None:
            print(f"  check-changelog: {seen} -- nothing to check, which is not a pass")
            return 2
        for finding in findings:
            print(f"  {finding}")
        bad, paragraphs, entries = bad + len(findings), paragraphs + seen, entries + items
    if listing:
        return 0
    if bad:
        print(f"check-changelog: FAILED ({bad} finding(s))")
        return 1
    print(f"  [Unreleased]: {paragraphs} paragraph(s) and {entries} entr{'y' if entries == 1 else 'ies'} in "
          f"{len(paths)} file(s), every sentence ended, every headline once")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
