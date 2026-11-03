#!/usr/bin/env python3
"""check-changelog.py -- every sentence of a CHANGELOG's [Unreleased] section is written to its end (installed at
<project>/scripts/, byte-identical in qb and the three qbm repositories; doc-lint.sh runs it on CHANGELOG.md).

An entry is written, rewritten and moved by hand, and nothing read the section whole: qbm-http's [Unreleased] carried,
from 2026-10-29 to 2026-11-02, an entry cut off in mid-sentence -- "a session class that declares an `on()` of its
own (the" -- left beside its rewrite by the edit that should have replaced it, and doc-lint, cite-check, llm-guard and
verify.sh all passed it (Huly QB-1221). The release body is generated from these entries. In [Unreleased]:

  * every paragraph ends its sentence. A paragraph is a run of non-blank lines that a blank line, a heading, a list
    item, a table or a block quote ends -- an item an entry or one of its sub-items, a numbered one only where it can
    open a list (`1.`, `1)`, or a list still open: an entry, or prose at the list's depth, closes it), as Markdown
    reads it -- so an entry cut short and followed at once by the next one is still a paragraph that ends
    unfinished. Past any closing mark (`)`, `]`, `*`, `_`, `~`, a backtick, a quote), its last line ends on `.`, `!`,
    `?` or `…`; on `:` only where what it introduces follows -- a table, a code block, a block quote, or an item or a
    paragraph deeper than the paragraph (indented past the item that opens it, or, in an entry's body, at least as
    deep as the body), which the next entry never is, and a break, a reference or a line of HTML between them
    settles nothing; on `;` only inside an indented list, where another of its items follows, at any depth. What
    ends otherwise and is no sentence was listed from every section of the six CHANGELOGs of the superproject (1 190
    paragraphs on 2026-11-03, not one other ending): a table row, a link reference definition
    (`[3.0.0]: https://...`), a thematic break (`---`), a fenced code block, read from its fence to the fence that
    closes it -- and a line of HTML tags and elements with no text outside an element (`<br>`,
    `<details><summary>More</summary>`) or of closed comments (`<!-- note -->`), which none of them holds, read with
    its closed comments set aside. HTML comments are read no
    further: a line that is only closed comments is a block of its own, which ends the paragraph before it and
    settles nothing; one that opens a line with text after it ends the paragraph before it; one closed inside a line
    is set aside; one over several lines is text, which a paragraph must end, and one that opens a line and closes
    nowhere in the section is a finding, for it hides the rest of the file. No CHANGELOG writes a comment. A
    paragraph whose last lines are such lines ends on the last line that is not;
  * every entry opens on `- ` and a bold headline, as the release body lists it (one opened with `*`, `+` or a tab is
    folded into the entry before it), and none is cut off -- every bracket or parenthesis it opens is closed, past its
    code spans and escapes, no backtick is left open, and its `**` neither opens nor closes on a space nor closes
    before a letter or a digit, or before a word that holds an odd number of `**` past its code spans (a code span
    or an `_emphasis_` read whole, as part of the word): on the bold of another span. All 993
    headlines of the corpus on 2026-11-03 pass (a closing bracket with nothing open is set aside: `[0, n)`, `a) b)`);
  * no text stands before the first entry of a section or a subsection that holds entries, which the release body
    cannot place;
  * no two entries open on the same bold headline: an entry written twice, as one is in qbm-http's published
    [3.3.0]. Headlines that only read alike are left alone -- parallel entries are written so, and the incident above
    is the cut-off sentence;
  * there is one [Unreleased] heading, outside code blocks.

An entry and its headline are read as dev/agent/release-gate.sh's release_notes() reads them to write the release
body -- the section runs from `## [Unreleased]` to the next `## [`, its `### ` subsections split it, an entry opens
with a line `- `, its headline is the `**...**` it opens with -- and `--headlines` lists them (path, line, `bold` or
`none`, the headline), for the superproject's battery to compare with what release_notes() publishes. This is not
the release gate: the length of a headline and the size of the release body are the gate's to judge.

Exit 0 = clean, 1 = at least one finding, 2 = nothing to check: a missing file, or one with no [Unreleased] section
or no paragraph in it. Usage, from the project root:  python3 scripts/check-changelog.py [--headlines] [FILE ...]
(default: ./CHANGELOG.md)
"""
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
QUOTE = re.compile(r"[ \t]*>")
EXEMPT = re.compile(r"[ \t]*(?:\|.*|\[[^\]]+\]:[ \t]+\S.*|(?:-[ \t]*){3,}|(?:\*[ \t]*){3,}|(?:_[ \t]*){3,}"
                    r"|(?:[ \t]*(?:<([A-Za-z][A-Za-z0-9-]*)[^<>]*>[^<>]*</\1[ \t]*>|<[^<>]*>))+[ \t]*)$")
CLOSERS = r"[)\]*_~`\"'”’»]*$"
ENDS, INTRODUCES, ENUMERATES = re.compile(r"[.!?…]" + CLOSERS), re.compile(r":" + CLOSERS), re.compile(r";" + CLOSERS)
HEADLINE = re.compile(r"- \*\*(.+?)\*\*", re.S)
CODE_SPAN = re.compile(r"(`+)(?:(?!\1).)+\1")
LINE_COMMENT = re.compile(r"<!--.*?-->")
AFTER = re.compile(r"(?:(`+)(?:(?!\1).)+\1|_[^_\n]*_|\S)*")  # the word a bold runs into, spans whole


def fenced(lines):
    """For each line, whether it is inside a fenced code block or is one of its fences."""
    out, fence = [], None
    for line in lines:
        if fence:
            closing = line.strip()
            if closing.startswith(fence) and closing == fence[0] * len(closing):
                fence = None
            out.append(True)
            continue
        opened = FENCE.match(line)
        fence = opened.group(1) if opened else None
        out.append(bool(opened))
    return out


def unreleased(text):
    """(the first [Unreleased] body, the line its first line is, how many [Unreleased] headings stand outside a code
    block), or None."""
    section = SECTION.search(text)
    if not section:
        return None
    lines = text.split("\n")
    headings = sum(1 for line, inside in zip(lines, fenced(lines)) if not inside and line.startswith("## [Unreleased]"))
    return section.group(1), text.count("\n", 0, section.start(1)) + 1, headings


def headlines(body, first):
    """(entries, leads), split and read exactly as release_notes() does: subsections at `### `, entries at `- ` (inside
    a fence or not), a headline the `**...**` an entry opens with, its whitespace runs made one space. An entry is
    (line, headline, the bold's text as written), or (line, None, None) without one; a lead, the text of a section or
    a subsection before its first entry, which release_notes() refuses once the section holds an entry or a
    subsection, is (line, its first line). An entry also carries the word its bold runs into (a code span or an
    `_emphasis_` read whole), or ""."""
    entries, leads, at = [], [], 0
    structured = re.search(r"(?m)^(- |### )", body)
    for part in re.split(r"(?m)^(?=### )", body):
        skip = len(part.partition("\n")[0]) + 1 if part.startswith("### ") else 0
        lead, *items = re.split(r"(?m)^(?=- )", part[skip:])
        if structured and lead.strip():
            start = at + skip + len(lead) - len(lead.lstrip())
            leads.append((first + body.count("\n", 0, start), lead.strip().splitlines()[0]))
        pos = at + skip + len(lead)
        for item in items:
            head, number = HEADLINE.match(item), first + body.count("\n", 0, pos)
            entries.append((number, " ".join(head.group(1).split()), head.group(1),
                            AFTER.match(item, head.end()).group(0)) if head else (number, None, None, ""))
            pos += len(item)
        at += len(part)
    return entries, leads


def balanced(head):
    """Whether every bracket or parenthesis a headline opens is closed, its code spans and escapes (`\\(`) set aside,
    and no backtick is left open. Either kind closes either (`[0, n)`), and a closing one with nothing open is set
    aside (`a) b)`)."""
    text = re.sub(r"\\.", "", CODE_SPAN.sub("", head))
    depth = 0
    for c in text:
        if c in "([":
            depth += 1
        elif c in ")]" and depth:
            depth -= 1
    return not depth and "`" not in text


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
    block, fence, pending = [], None, None  # pending: (line, ':' or ';', an item's least indent, the paragraph's, text)
    listing = None                          # the indent of the numbered list open, or None

    def close():
        """End the paragraph in `block`: its last line that is not a table row, a link reference, a break or a line of
        HTML, each read with its closed comments set aside."""
        nonlocal paragraphs, pending
        if block:
            paragraphs += 1
            last = next(((n, l) for n, l in reversed(block) if not EXEMPT.match(LINE_COMMENT.sub("", l))), None)
            if last:
                number, written = last
                tail = LINE_COMMENT.sub("", written).rstrip()  # a comment closed on its line is set aside
                opener = block[0][1]
                indent, item = len(opener) - len(opener.lstrip()), BULLET.match(opener) or ORDERED.match(opener)
                if INTRODUCES.search(tail):  # the least indent of an item it introduces
                    pending = (number, ":", indent + 1 if item else max(indent, 1), indent, written.rstrip())
                elif ENUMERATES.search(tail) and item and indent:
                    pending = (number, ";", 1, indent, written.rstrip())
                elif not ENDS.search(tail):
                    findings.append(f"{path}:{number}: [Unreleased] a paragraph ends before its sentence does: "
                                    f"...{written.rstrip()[-70:]}")
            block.clear()

    def follows(kind, indent=0):
        """What opens next -- its kind and its indent -- settles a paragraph that ended on `:` or `;`: a `:`
        introduces a table, a code block, a quote, an item deeper than the paragraph or a paragraph indented past it;
        a `;` is followed by another item of an indented list, at whatever depth (a sub-list, or the next item of the
        list a sub-list closes)."""
        nonlocal pending
        if pending:
            number, mark, own, deep, tail = pending
            if mark == ":" and not (kind in ("table", "fence", "quote") or kind in ("item", "text") and
                                    indent >= own) or mark == ";" and not (kind == "item" and indent >= own):
                what = "nothing it introduces" if mark == ":" else "no other item of its list"
                findings.append(f"{path}:{number}: [Unreleased] a paragraph ends on `{mark}` and {what} follows: "
                                f"...{tail[-70:]}")
            pending = None

    lines = body.split("\n")
    for number, line in enumerate(lines, first):
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
        indent, bare = len(line) - len(line.lstrip()), LINE_COMMENT.sub("", line)
        if BULLET.match(line) and not indent and not line.startswith("- ") and not EXEMPT.match(line):
            opener = "`-` and a tab" if line.startswith("-\t") else f"`{line[0]}`"
            findings.append(f"{path}:{number}: [Unreleased] an entry opened with {opener}, which the release body does "
                            f"not read as an entry (it opens on `- `)")
        heading = HEADING.match(line)
        if BULLET.match(line) and not indent or not block and listing is not None and indent <= listing and \
                not BULLET.match(line) and not ORDERED.match(line):
            listing = None                               # an entry, or prose at the list's depth: the list is over
        ordered = ORDERED.match(line)
        item = BULLET.match(line) or ordered and (ordered.group(1) == "1" or listing is not None)
        table = TABLE.match(line) and not (block and TABLE.match(block[-1][1]))
        quote = QUOTE.match(line) and not (block and QUOTE.match(block[-1][1]))
        comment = line.lstrip().startswith("<!--")       # interrupts a paragraph, as Markdown reads it
        if heading or item or table or quote or comment:
            close()
        if comment and "-->" not in "\n".join(lines[number - first:])[line.index("<!--") + 2:]:  # `<!-->` closes
            findings.append(f"{path}:{number}: [Unreleased] an HTML comment opens a line and closes nowhere in the "
                            f"section: it hides the rest of the file")
        if comment and not bare.strip():
            continue                                     # a line of closed comments: a block of its own
        if not block and not (EXEMPT.match(bare) and not TABLE.match(bare)):  # a break, a reference or a line of HTML
            follows("heading" if heading else "item" if item else "table" if TABLE.match(line) else   # settles nothing
                    "quote" if QUOTE.match(line) else "text", indent)
        if heading:
            listing = None
            continue
        if ordered and item:
            listing = indent if listing is None else min(listing, indent)
        block.append((number, line))
    close()
    follows("end")
    if fence:
        findings.append(f"{path}: [Unreleased] a code fence opens and never closes")
    if not paragraphs:
        return None, f"{path}: no paragraph in [Unreleased]", 0
    (entries, leads), seen = headlines(body, first), {}
    for number, lead in leads:
        findings.append(f"{path}:{number}: [Unreleased] text before the first entry of its section, which the release "
                        f"body cannot place: {lead[:90]}")
    for number, head, written, after in entries:
        if head is None:
            findings.append(f"{path}:{number}: [Unreleased] an entry with no bold headline -- the release body lists "
                            f"each entry by its headline (or its `**` never closes)")
            continue
        if not balanced(head):
            findings.append(f"{path}:{number}: [Unreleased] a headline cut off -- a bracket or a backtick is left "
                            f"open: {head[:90]}")
        elif written != written.strip() or after[:1].isalnum() or CODE_SPAN.sub("", after).count("**") % 2:
            findings.append(f"{path}:{number}: [Unreleased] a headline cut off -- its `**` opens or closes on a space "
                            f"or closes inside a word, on the bold of another span: {head[:90]}")
        if head in seen:
            findings.append(f"{path}:{number}: [Unreleased] the same headline as at line {seen[head]}: {head[:90]}")
        seen.setdefault(head, number)
    return findings, paragraphs, len(entries)


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")  # a console that cannot print a character names it
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
            for number, head, *_ in headlines(found[0], found[1])[0]:
                print(f"{path}\t{number}\t{'bold' if head is not None else 'none'}\t{head or ''}")
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
