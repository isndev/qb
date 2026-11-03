#!/usr/bin/env python3
"""check-changelog.py -- every sentence of a CHANGELOG's [Unreleased] section is written to its end (installed at
<project>/scripts/, byte-identical in qb and the three qbm repositories; doc-lint.sh runs it on CHANGELOG.md).

An entry is written, rewritten and moved by hand, and nothing read the section whole: qbm-http's [Unreleased] carried,
from 2026-10-29 to 2026-11-02, an entry cut off in mid-sentence -- "a session class that declares an `on()` of its
own (the" -- left beside its rewrite by the edit that should have replaced it, and doc-lint, cite-check, llm-guard and
verify.sh all passed it (Huly QB-1221). The release body is generated from these entries. In [Unreleased]:

  * every paragraph ends its sentence: past any closing mark (`)`, `]`, `*`, `_`, a backtick, a quote) its last
    character is `.`, `!`, `?` or `:` (a `:` introduces the table or the code block after it). A paragraph is a run
    of non-blank lines, and a heading or a list item -- an entry or one of its sub-items -- opens a new one, so an
    entry cut short and followed at once by the next one is still one paragraph that ends unfinished. What ends
    without that punctuation in the published sections, and so is no paragraph here, was listed from the six
    CHANGELOGs, published sections included (1 171 paragraphs at develop 03d8c417, not one other ending): a
    table row, a link reference definition
    (`[3.0.0]: https://...`), a thematic break (`---`), and a fenced code block, read from fence to fence;
  * no two entries open on the same bold headline: the second copy of an entry rewritten beside the first.

An entry and its headline are read as dev/agent/release-gate.sh's release_notes() reads them to write the release
body -- the section runs from `## [Unreleased]` to the next `## [`, its `### ` subsections split it, an entry opens
with a line `- `, its headline is the `**...**` it opens with -- and `--headlines` lists them, for the superproject's
battery to compare with what release_notes() publishes.

Exit 0 = clean, 1 = at least one finding, 2 = nothing to check (a missing file, no [Unreleased] section, no paragraph).
Usage, from the project root:  python3 scripts/check-changelog.py [--headlines] [CHANGELOG.md ...]
                               (default: ./CHANGELOG.md)
"""
import io
import os
import re
import sys

SECTION = re.compile(r"^## \[Unreleased\][^\n]*\n(.*?)(?=^## \[|\Z)", re.S | re.M)
FENCE = re.compile(r"[ \t]*(```|~~~)")
HEADING = re.compile(r"#{1,6}[ \t]")
ITEM = re.compile(r"[ \t]*(?:[-*+]|\d+[.)])[ \t]")
EXEMPT = re.compile(r"[ \t]*(?:\|.*|\[[^\]]+\]:[ \t]+\S.*|(?:-[ \t]*){3,}|(?:\*[ \t]*){3,}|(?:_[ \t]*){3,})$")
ENDS = re.compile(r"[.!?:][)\]*_`\"'”’»]*$")
HEADLINE = re.compile(r"- \*\*(.+?)\*\*", re.S)


def unreleased(path):
    """(the [Unreleased] body, the line number its first line has), or (None, why)."""
    try:
        text = io.open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    except OSError as error:
        return None, f"{path}: {error.strerror}"
    section = SECTION.search(text)
    if not section:
        return None, f"{path}: no [Unreleased] section"
    return section.group(1), text.count("\n", 0, section.start(1)) + 1


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


def check(path):
    """(findings, paragraphs, entries) of one CHANGELOG's [Unreleased]; findings is None, with why, if there is none."""
    body, first = unreleased(path)
    if body is None:
        return None, first, 0
    findings, paragraphs, block, fence = [], 0, [], None

    def close():
        nonlocal paragraphs
        if block:
            paragraphs += 1
            number, last = block[-1]
            if not EXEMPT.match(last) and not ENDS.search(last.rstrip()):
                findings.append(f"{path}:{number}: [Unreleased] a paragraph ends before its sentence does: "
                                f"...{last.rstrip()[-70:]}")
            block.clear()

    for number, line in enumerate(body.split("\n"), first):
        if fence:
            if line.lstrip().startswith(fence):
                fence = None
            continue
        opened = FENCE.match(line)
        heading = HEADING.match(line)
        if opened or heading or not line.strip() or ITEM.match(line):
            close()
        if opened:
            fence = opened.group(1)
        elif line.strip() and not heading:
            block.append((number, line))
    close()
    if fence:
        findings.append(f"{path}: [Unreleased] a code fence opens and never closes")
    entries, seen = headlines(body, first), {}
    for number, head in entries:
        if head is None:
            continue
        if head in seen:
            findings.append(f"{path}:{number}: [Unreleased] a headline written twice (first at line {seen[head]}): "
                            f"{head[:90]}")
        else:
            seen[head] = number
    return findings, paragraphs, len(entries)


def main(argv):
    listing = "--headlines" in argv
    paths = [a for a in argv if a != "--headlines"] or [os.path.join(os.getcwd(), "CHANGELOG.md")]
    bad = paragraphs = entries = 0
    for path in paths:
        if listing:
            body, first = unreleased(path)
            if body is None:
                print(f"  check-changelog: {first} -- nothing to list")
                return 2
            for number, head in headlines(body, first):
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
    if not paragraphs:
        print(f"  check-changelog: no paragraph in the [Unreleased] of {len(paths)} file(s) -- nothing to check")
        return 2
    if bad:
        print(f"check-changelog: FAILED ({bad} finding(s))")
        return 1
    print(f"  [Unreleased]: {paragraphs} paragraph(s) and {entries} entr{'y' if entries == 1 else 'ies'} in "
          f"{len(paths)} file(s), every sentence ended, every headline once")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
