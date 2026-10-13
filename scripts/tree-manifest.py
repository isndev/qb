#!/usr/bin/env python3
"""Digest a directory's paths, types, permission modes and file/symlink contents."""
import hashlib
import json
import os
from pathlib import Path
import stat
import sys


def tree_digest(root: Path) -> str:
    if not root.is_dir():
        raise ValueError(f"not a directory: {root}")
    entries = []
    paths = [root]
    for directory, dirs, files in os.walk(root, followlinks=False):
        paths.extend(Path(directory) / name for name in dirs + files)
    for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix()):
        info = path.lstat()
        kind = stat.S_IFMT(info.st_mode)
        if stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            content = digest.hexdigest()
        elif stat.S_ISLNK(info.st_mode):
            content = os.readlink(path)
        else:
            content = ""
        entries.append((path.relative_to(root).as_posix(), kind, stat.S_IMODE(info.st_mode), content))
    return hashlib.sha256(json.dumps(entries, ensure_ascii=True, separators=(",", ":")).encode()).hexdigest()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: tree-manifest.py DIRECTORY")
    print(tree_digest(Path(sys.argv[1])))
