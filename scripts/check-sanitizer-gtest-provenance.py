#!/usr/bin/env python3
"""Prove sanitized CI compiles its pinned GoogleTest sources with the same flags."""

import json
import re
import subprocess
import sys
from pathlib import Path


SOURCES = ("gtest-all.cc", "gmock-all.cc")


def cache_value(cache: str, key: str) -> str | None:
    match = re.search(rf"^{re.escape(key)}:[^=]*=(.*)$", cache, re.MULTILINE)
    return match.group(1) if match else None


def system_policy_errors(cache: str) -> list[str]:
    if cache_value(cache, "QB_USE_SYSTEM_GTEST") != "OFF":
        return ["QB_USE_SYSTEM_GTEST must be OFF in sanitizer CI"]
    return []


def source_path(entry: dict) -> Path:
    source = Path(entry["file"])
    if not source.is_absolute():
        source = Path(entry["directory"]) / source
    return source.resolve()


def command(entry: dict) -> str:
    return entry.get("command") or " ".join(entry.get("arguments", []))


def inspect(entries: list[dict], source_root: Path, flag: str) -> list[str]:
    errors = []
    for name in SOURCES:
        matches = [entry for entry in entries if Path(entry["file"]).name == name]
        if len(matches) != 1:
            errors.append(f"{name}: expected one compile entry, found {len(matches)}")
            continue
        entry = matches[0]
        source = source_path(entry)
        if source_root not in source.parents:
            errors.append(f"{name}: source is not the fetched GoogleTest: {source}")
        if flag not in command(entry).split():
            errors.append(f"{name}: missing sanitizer compile flag {flag}")
    return errors


def git_rev(source_root: Path, ref: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(source_root), "rev-parse", "--verify", ref],
        text=True,
        stderr=subprocess.PIPE,
    ).strip()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check-sanitizer-gtest-provenance.py BUILD_DIR address,undefined|thread", file=sys.stderr)
        return 2
    build_dir = Path(sys.argv[1]).resolve()
    sanitizer = sys.argv[2]
    if sanitizer not in ("address,undefined", "thread"):
        print(f"unsupported sanitizer profile: {sanitizer}", file=sys.stderr)
        return 2
    flag = f"-fsanitize={sanitizer}"
    source_root = (build_dir / "_deps" / "googletest-src").resolve()
    try:
        entries = json.loads((build_dir / "compile_commands.json").read_text())
        cache = (build_dir / "CMakeCache.txt").read_text()
    except (OSError, ValueError) as exc:
        print(f"cannot inspect sanitizer configure: {exc}", file=sys.stderr)
        return 2

    if errors := system_policy_errors(cache):
        for error in errors:
            print(f"::error::{error}")
        return 1
    tag = cache_value(cache, "QB_GOOGLETEST_GIT_TAG")
    if not tag or not source_root.is_dir():
        print(f"::error::pinned GoogleTest checkout missing: tag={tag!r}, source={source_root}")
        return 1
    try:
        head = git_rev(source_root, "HEAD")
        pinned = git_rev(source_root, f"{tag}^{{commit}}")
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"::error::cannot resolve pinned GoogleTest revision: {exc}")
        return 1
    if head != pinned:
        print(f"::error::GoogleTest checkout {head} differs from {tag} ({pinned})")
        return 1

    errors = inspect(entries, source_root, flag)
    if errors:
        for error in errors:
            print(f"::error::{error}")
        return 1
    print(f"GoogleTest provenance: {tag} at {head}; {len(SOURCES)}/{len(SOURCES)} source units carry {flag}")

    # Controls operate on copies of the real compile entries. A missing sanitizer
    # flag or a system-package source must make this same predicate reject them.
    forced_cache, planted = re.subn(
        r"^(QB_USE_SYSTEM_GTEST:[^=]*=)OFF$", r"\g<1>ON", cache, count=1, flags=re.MULTILINE
    )
    if planted != 1 or not system_policy_errors(forced_cache):
        print("::error::negative control accepted QB_USE_SYSTEM_GTEST=ON")
        return 1
    print("CAUGHT QB_USE_SYSTEM_GTEST=ON")

    no_flag = [dict(entry) for entry in entries]
    for entry in no_flag:
        if Path(entry["file"]).name in SOURCES:
            if "command" in entry:
                entry["command"] = entry["command"].replace(flag, "", 1)
            else:
                entry["arguments"] = [arg for arg in entry["arguments"] if arg != flag]
    if not any("missing sanitizer compile flag" in error for error in inspect(no_flag, source_root, flag)):
        print("::error::negative control missed uninstrumented GoogleTest source")
        return 1
    print("CAUGHT uninstrumented GoogleTest source")

    system_source = [dict(entry) for entry in entries]
    for entry in system_source:
        if Path(entry["file"]).name in SOURCES:
            entry["file"] = str(Path("/usr/src/googletest") / Path(entry["file"]).name)
    if not any("source is not the fetched GoogleTest" in error for error in inspect(system_source, source_root, flag)):
        print("::error::negative control accepted system GoogleTest source")
        return 1
    print("CAUGHT system GoogleTest source")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
