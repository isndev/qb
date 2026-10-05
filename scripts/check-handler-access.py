#!/usr/bin/env python3
"""check-handler-access.py — an `on(Evt)` handler for an event the framework dispatches
through a detection gate must be one that gate can SEE.

WHY THIS EXISTS
---------------
The framework delivers its optional events — `disconnected`, `eos`, `pending_read`, `dispose`,
the WebSocket `close`/`ping`/`pong`, the HTTP/2 `Http2GoAwayEvent`, a redis consumer's `error`,
twenty-odd in all — only when a trait says the receiving class handles them:

    if constexpr (qb::has_on<_Derived, event::disconnected>)        // io<>, input<>, output<>
    if constexpr (has_method_on<Derived, void, Evt>::value)          // qbm-redis consumers
    if constexpr (qb::has_own_on<_Derived, acceptor, Evt>)           // the acceptor, tcp::server

A handler the trait cannot see is skipped with no diagnostic at all, and two shapes make it
invisible:

  1. ACCESS. The detection is a requires-expression written inside the `has_method_on` struct
     QB_DEFINE_METHOD_TRAIT generates (qb/src/qb/utility/type_traits.h), so it is
     access-checked THERE: a private or protected `on(Evt)` is seen only when its class
     befriends that struct — `friend struct has_method_on<Self, void, Evt>;`, or the whole
     template. Befriending the CRTP base that dispatches is the natural C++ reflex, and it is
     NOT enough. Measured (Huly QB-252, 2026-10-05): qbm-redis's `RedisCoroConsumer` declared
     its `on(disconnected)` private and befriended `RedisConsumer` — the handler that closes
     the receive queue never ran, so `while (auto m = co_await receive())` never ended on any
     disconnect; the qbm-http HTTP/1.1 and HTTP/2 servers declared theirs private and
     befriended the acceptor — `qb::has_own_on` (then unable to honour any friendship) did not
     see them, the acceptor threw instead, and the throw left the listening watcher armed: a
     client waiting in the backlog made the loop dispatch on every pass.
  2. SIGNATURE. A gate probes what it names — `has_on<D, E>` an rvalue `E`. A non-const
     lvalue-reference handler `on(E &)` cannot bind it, so it is invisible whatever its access.
     `.claude/rules/qb-io-async.md` documents the trap; the examples corpus still taught it:
     `examples/02-io/05-custom-protocol.cpp` declared both its `disconnected` handlers
     `on(event::disconnected &)`, so neither ever ran and its client could not leave its loop
     when the server went.

The compiler says nothing about either — the gate is an `if constexpr`, the handler simply
never runs. The suite says nothing unless a test drives that exact event to that exact class.

WHAT IT CHECKS
--------------
  * The GATED EVENTS are DERIVED from the framework source, never typed here: every
    `has_on<X, E…>`, `has_method_on<X, R, E…>` and `has_own_on<X, B, E>` written in the
    `--framework` roots (friend declarations and the traits' own definitions in
    `type_traits.h` excepted). The event is the first event argument's last `::` component;
    what the gate probes — rvalue, const lvalue, non-const lvalue — is read from its spelling.
    The run PRINTS the set; an empty set is a hard stop (this guard would pass vacuously).
  * For every class/struct body under the subject roots (`.h` and `.cpp`), every `on(` member
    declared at class scope, with the access in force (`class` defaults private, `struct`
    public) and the class's `friend struct has_method_on<…, Evt>` declarations:
      R1  a non-public `on(E…)` for a gated `E` its class does not befriend the detector for;
      R2  an `on(E &)` — non-const lvalue reference — for a gated `E` no gate probes as a
          non-const lvalue.
  * ESCAPE HATCH: `// handler-access: <reason>` on the handler's line or one of the two lines
    above it. The reason is MANDATORY. It is for a handler that is not reached through a gate
    at all — a watcher's own handler registered with the listener, or one a friend base calls
    directly — and saying which is the point of writing it.

WHAT IT DELIBERATELY DOES NOT CATCH
-----------------------------------
It is textual. Events are matched by their last name, so `qb::http::event::disconnected` and
`qb::io::async::event::disconnected` are one key — conservative in R1 (more handlers checked,
never fewer), and the reason R2 reads the probe category per name. An event reached through a
`using` alias of another spelling, or a gate written through a macro, is invisible to it. A
handler whose class grants access some other way (a friend function template specialisation of
the detector) is reported and needs the annotation. These are stated, not hidden behind a green.

ANTI-VACUOUS FLOORS
-------------------
Each subject root carries a `NAME:MIN` floor — the minimum number of `.h`/`.cpp` VISITED — per
root, never shared. `--min-handlers` floors the `on(` members actually parsed (a broken class
scanner reports as itself, not as "no findings"); `--min-gated` floors the derived event set.

USAGE
-----
    ./scripts/check-handler-access.py                       # qb alone, from the qb root
    ./scripts/check-handler-access.py --framework qb/src --framework qbm/http/src \
        qb/src:150 qbm/http/src:80 qb/tests:240 examples:138 --min-handlers=400 --min-gated=20

Exit status: 0 clean, 1 findings, 2 usage/IO error or a vacuous run.
"""

from __future__ import annotations

import os
import re
import sys

SKIP_DIRS = {".git", "build", "__pycache__", ".cache", "node_modules", ".venv", "vendor"}
SUFFIXES = (".h", ".cpp")

GATE_RE = re.compile(r"\b(has_on|has_method_on|has_own_on)\s*<")
FRIEND_EVT_RE = re.compile(r"friend\s+struct\s+(?:::)?has_method_on\s*<")
FRIEND_ALL_RE = re.compile(r"template\s*<[^<>;{}]*>\s*friend\s+struct\s+(?:::)?has_method_on\s*;")
HEAD_RE = re.compile(r"\b(class|struct)\s+(?:alignas\s*\([^)]*\)\s*)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{;()]*)?\{")
ACCESS_RE = re.compile(r"(public|private|protected)\s*:(?!:)")
HANDLER_RE = re.compile(r"\bon\s*\(")
ANNOTATION_RE = re.compile(r"handler-access:\s*(\S.*)")
# Words that end a type spelling without naming a parameter: `const Evt` has no parameter name.
TYPE_WORDS = {"const", "volatile", "typename", "struct", "class", "unsigned", "signed", "long", "short"}


def strip_comments(src: str) -> str:
    """Blank // and /* */ comments and string/char literals, preserving offsets and lines."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c == "R" and nxt == '"':
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i)
            end = n if end == -1 else end + len(m.group(1)) + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:end]))
            i = end
        elif c in "\"'":
            if c == "'" and i > 0 and (src[i - 1].isalnum() or src[i - 1] == "_"):
                out.append(c)  # digit separator: 1'000
                i += 1
                continue
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_angle(src: str, open_idx: int) -> int:
    """Index just past the `>` matching the `<` at open_idx, or -1. Skips (), [], {}."""
    depth, i, n = 0, open_idx, len(src)
    while i < n:
        c = src[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return i + 1
        elif c in "([{":
            close = {"(": ")", "[": "]", "{": "}"}[c]
            d2, j = 0, i
            while j < n:
                if src[j] == c:
                    d2 += 1
                elif src[j] == close:
                    d2 -= 1
                    if d2 == 0:
                        break
                j += 1
            i = j
        elif c == ";":
            return -1
        i += 1
    return -1


def match_paren(src: str, open_idx: int) -> int:
    depth, i, n = 0, open_idx, len(src)
    while i < n:
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def split_top_level(args: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in args:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def event_key(type_text: str) -> tuple[str, str]:
    """(event name, probe category) of one argument or parameter spelling.

    The name is the last `::` component with template arguments, cv and references dropped;
    the category is what a gate spelled that way probes, or what a handler parameter takes:
    "lvalue" (non-const `T&`), "const_lvalue" (`T const&` / `const T&`), "rvalue" (anything
    else: `T`, `T&&`)."""
    t = re.sub(r"\[\[[^\]]*\]\]", " ", type_text)  # [[maybe_unused]] and friends
    t = re.sub(r"\btypename\b", " ", t)
    t = t.split("=", 1)[0].strip()  # a default argument
    # Drop a trailing parameter NAME: an identifier glued to `&`/`*`/`>` (`&&e`, `&event`), or one
    # separated by whitespace from a preceding type token that is not itself a type keyword
    # (`message msg` loses `msg`; `const Evt`, `unsigned int`, `long long` keep theirs).
    m = re.search(r"([&*>])\s*([A-Za-z_]\w*)$", t)
    if m:
        t = t[: m.start(2)].rstrip()
    else:
        m = re.search(r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)$", t)
        if m and m.group(1) not in TYPE_WORDS:
            t = t[: m.start(2)].rstrip()
    is_const = bool(re.search(r"\bconst\b", t))
    stripped = t.replace("&&", " RR ").strip()
    if " RR " in stripped or stripped.endswith("RR"):
        cat = "rvalue"
    elif "&" in stripped:
        cat = "const_lvalue" if is_const else "lvalue"
    else:
        cat = "rvalue"
    core = re.sub(r"\bconst\b|\bvolatile\b|&|\*|\bRR\b", " ", stripped)
    depth, flat = 0, []
    for ch in core:  # drop template arguments: Http2FrameData<PingFrame> -> Http2FrameData
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif depth == 0:
            flat.append(ch)
    name = "".join(flat).strip().split("::")[-1].strip()
    return name, cat


def walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            if fn.endswith(SUFFIXES):
                yield os.path.join(dirpath, fn)


def derive_gates(framework_roots: list[str]) -> dict[str, set[str]]:
    """event name -> the probe categories its gates use, over every framework gate."""
    gates: dict[str, set[str]] = {}
    for root in framework_roots:
        for path in walk(root):
            if os.path.basename(path) == "type_traits.h":
                continue  # the traits' own definitions, not uses
            try:
                src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            except OSError:
                continue
            for m in GATE_RE.finditer(src):
                if re.search(r"friend\s+struct\s+(?:::)?$", src[max(0, m.start() - 40) : m.start()]):
                    continue
                close = match_angle(src, m.end() - 1)
                if close < 0:
                    continue
                args = split_top_level(src[m.end() : close - 1])
                first_event = {"has_on": 1, "has_method_on": 2, "has_own_on": 2}[m.group(1)]
                if len(args) <= first_event:
                    continue
                name, cat = event_key(args[first_event])
                if name:
                    gates.setdefault(name, set()).add(cat)
    return gates


def scan_file(path: str, gates: dict[str, set[str]]) -> tuple[list[str], int]:
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ([f"{path}: unreadable"], 0)
    src = strip_comments(raw)
    raw_lines = raw.split("\n")
    findings: list[str] = []
    handlers = 0
    for head in HEAD_RE.finditer(src):
        kind, cls = head.group(1), head.group(2)
        body_start = head.end()  # just past the class body's `{`
        access = "public" if kind == "struct" else "private"
        friends: set[str] = set()
        befriends_all = False
        decls: list[tuple[int, str, str]] = []  # (offset, access, first parameter text)
        depth, i, n = 1, body_start, len(src)
        while i < n and depth > 0:
            c = src[i]
            if c == "{":
                depth += 1
                i += 1
                continue
            if c == "}":
                depth -= 1
                i += 1
                continue
            if depth == 1:
                am = ACCESS_RE.match(src, i)
                if am and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
                    access = am.group(1)
                    i = am.end()
                    continue
                fa = FRIEND_ALL_RE.match(src, i)
                if fa:
                    befriends_all = True
                    i = fa.end()
                    continue
                fe = FRIEND_EVT_RE.match(src, i)
                if fe:
                    close = match_angle(src, fe.end() - 1)
                    if close > 0:
                        args = split_top_level(src[fe.end() : close - 1])
                        if len(args) >= 3:
                            friends.add(event_key(args[2])[0])
                        i = close
                        continue
                hm = HANDLER_RE.match(src, i)
                if hm and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] in "_.>:")):
                    close = match_paren(src, hm.end() - 1)
                    if close > 0:
                        params = split_top_level(src[hm.end() : close - 1])
                        after = src[close : close + 40].lstrip()
                        # a declaration or definition, not a call in an initializer
                        if params and (after[:1] in ("{", ";") or after.startswith(("const", "noexcept", "override", "final", "->", "&"))):
                            decls.append((i, access, params[0]))
                        i = close
                        continue
            i += 1
        for off, acc, first in decls:
            handlers += 1
            name, cat = event_key(first)
            if name not in gates:
                continue
            line = src.count("\n", 0, off) + 1
            note = " ".join(raw_lines[max(0, line - 3) : line])
            ann = ANNOTATION_RE.search(note)
            if ann and ann.group(1).strip():
                continue
            if acc != "public" and not befriends_all and name not in friends:
                findings.append(
                    f"{path}:{line}: [R1] {cls}::on({first.strip()}) is {acc} and {cls} does not befriend "
                    f"the detector, so the gate on `{name}` cannot see it and skips it silently. Add "
                    f"`friend struct has_method_on<{cls}, void, <the event type>>;` (befriending the base "
                    f"that dispatches is not enough), or `// handler-access: <reason>` if no gate reaches it."
                )
            if cat == "lvalue" and "lvalue" not in gates[name]:
                findings.append(
                    f"{path}:{line}: [R2] {cls}::on({first.strip()}) takes a non-const lvalue reference; "
                    f"the gates on `{name}` probe {', '.join(sorted(gates[name]))}, which it cannot bind, "
                    f"so it is never called. Take `&&` or `const &`."
                )
    return findings, handlers


def default_args() -> list[str]:
    qb_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Measured at 3.3.0 on qb alone: 180 + 273 files, 534 on() members, 10 gated events.
    return [
        "--framework", os.path.join(qb_root, "src"),
        f"{os.path.join(qb_root, 'src')}:150", f"{os.path.join(qb_root, 'tests')}:240",
        "--min-handlers=480", "--min-gated=8",
    ]


def main() -> int:
    argv = sys.argv[1:] or default_args()
    framework_roots: list[str] = []
    specs: list[str] = []
    min_handlers = 1
    min_gated = 1
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--framework":
            i += 1
            if i >= len(argv):
                print("usage error: --framework needs a directory", file=sys.stderr)
                return 2
            framework_roots.append(argv[i])
        elif a.startswith("--min-handlers="):
            min_handlers = int(a.split("=", 1)[1])
        elif a.startswith("--min-gated="):
            min_gated = int(a.split("=", 1)[1])
        elif a.startswith("--"):
            print(f"usage error: unknown option {a}", file=sys.stderr)
            return 2
        else:
            specs.append(a)
        i += 1

    if not framework_roots:
        print("usage error: at least one --framework <dir> is required", file=sys.stderr)
        return 2
    for r in framework_roots + [s.rsplit(":", 1)[0] for s in specs]:
        if not os.path.isdir(r):
            print(f"::error::no such root: {r}", file=sys.stderr)
            return 2

    gates = derive_gates(framework_roots)
    if len(gates) < max(1, min_gated):
        print(
            f"::error::{len(gates)} gated event(s) derived from {', '.join(framework_roots)} "
            f"(floor {max(1, min_gated)}) — this guard would pass vacuously. Either the framework "
            "changed shape or the gate scan broke; refusing to report a green.",
            file=sys.stderr,
        )
        return 2
    print(f"gated events ({len(gates)}, derived from the framework's has_on / has_method_on / has_own_on uses):")
    print("  " + ", ".join(f"{k}[{'/'.join(sorted(v))}]" for k, v in sorted(gates.items())))

    findings: list[str] = []
    total_visited = 0
    total_handlers = 0
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
        visited = 0
        handlers = 0
        for path in walk(root):
            visited += 1
            f, h = scan_file(path, gates)
            findings.extend(f)
            handlers += h
        print(f"  {root}: {visited} sources+headers visited (floor {floor}), {handlers} on() members parsed")
        if visited < floor:
            print(f"::error::{root} visited {visited} sources, below its floor of {floor}", file=sys.stderr)
            return 2
        total_visited += visited
        total_handlers += handlers

    if total_handlers < min_handlers:
        print(
            f"::error::only {total_handlers} on() members parsed across {total_visited} sources "
            f"(floor {min_handlers}); the class scanner is not working, so a clean result would be vacuous.",
            file=sys.stderr,
        )
        return 2

    if findings:
        for f in findings:
            print(f"::error::{f}", file=sys.stderr)
        print(f"\n{len(findings)} finding(s).", file=sys.stderr)
        return 1

    print(f"OK: {total_visited} sources+headers, {total_handlers} on() members, {len(gates)} gated events, 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
