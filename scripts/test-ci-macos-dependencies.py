#!/usr/bin/env python3
"""Run the macOS dependency selector against an isolated, stateful fake brew."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from typing import Optional, Tuple


DEFAULT_SCRIPT = Path(__file__).resolve().with_name("ci-install-macos-dependencies.sh")

FAKE_BREW = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

state_path = Path(os.environ["FAKE_BREW_STATE"])
state = json.loads(state_path.read_text())
prefix = Path(os.environ["FAKE_BREW_PREFIX"])
args = sys.argv[1:]
with Path(os.environ["FAKE_BREW_LOG"]).open("a") as out:
    out.write(json.dumps(args) + "\n")

def save():
    state_path.write_text(json.dumps(state))
    helper = prefix / "opt/libngtcp2/lib/libngtcp2_crypto_ossl.dylib"
    helper.parent.mkdir(parents=True, exist_ok=True)
    if state.get("helper"):
        helper.touch()
    else:
        helper.unlink(missing_ok=True)

if args[0] == "deps":
    deps = state.get("deps_override", [state.get("helper_formula", "openssl@4")])
    print("\n".join(deps))
    if state.get("deps_error"):
        sys.exit(3)
elif args[0] == "install":
    if state.get("install_error"):
        sys.exit(4)
    if "libngtcp2" in args[1:]:
        state["helper_formula"] = state.get("upgrade_to") or state.get("helper_formula")
        state["helper"] = state.get("helper_after_install", True)
        if state["helper"]:
            state.setdefault("openssl_installed", []).append(state["helper_formula"])
    state.setdefault("openssl_installed", []).extend(
        a for a in args[1:] if a == "openssl" or a.startswith("openssl@")
    )
    save()
elif args[0] == "--prefix":
    print(prefix if len(args) == 1 else prefix / "opt" / args[1])
elif args[:2] == ["list", "--versions"]:
    name = args[2]
    if name == "libngtcp2" and state.get("helper"):
        print("libngtcp2 1.25.0")
    elif name in state.get("openssl_installed", []):
        print(f"{name} 4.0.2" if name in ("openssl", "openssl@4") else f"{name} 3.6.4")
    else:
        sys.exit(1)
else:
    print(f"unsupported fake brew call: {args}", file=sys.stderr)
    sys.exit(9)
'''


def run(script: Path, initial: dict, profile: str = "build") -> Tuple[int, dict, list, str]:
    with tempfile.TemporaryDirectory(prefix="qb-openssl-selector-") as tmp:
        tmp = Path(tmp)
        bin_dir = tmp / "bin"
        bin_dir.mkdir()
        brew = bin_dir / "brew"
        brew.write_text(FAKE_BREW)
        brew.chmod(0o755)
        prefix = tmp / "homebrew"
        helper = prefix / "opt/libngtcp2/lib/libngtcp2_crypto_ossl.dylib"
        helper.parent.mkdir(parents=True)
        if initial.get("helper"):
            helper.touch()
        state = tmp / "state.json"
        state.write_text(json.dumps(initial))
        calls = tmp / "brew-calls.jsonl"
        exported = tmp / "github-env"
        env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}",
                   GITHUB_ENV=str(exported), FAKE_BREW_STATE=str(state),
                   FAKE_BREW_PREFIX=str(prefix), FAKE_BREW_LOG=str(calls))
        result = subprocess.run(["bash", str(script), "--profile", profile],
                                env=env, text=True, capture_output=True, timeout=20)
        values = dict(line.split("=", 1) for line in exported.read_text().splitlines()) if exported.exists() else {}
        commands = [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []
        return result.returncode, values, commands, result.stdout + result.stderr


def check(script: Path, name: str, initial: dict, selected: Optional[str],
          profile: str = "build") -> None:
    rc, values, calls, output = run(script, initial, profile)
    if selected is None:
        assert rc != 0 and not values, (name, rc, values, output)
    else:
        assert rc == 0, (name, rc, output)
        prefix = values["OPENSSL_EXECUTABLE"].removesuffix("/bin/openssl")
        assert prefix.endswith(f"/opt/{selected}"), (name, prefix)
        assert values["CMAKE_PREFIX_PATH"].split(";")[0] == prefix, name
        assert values["PKG_CONFIG_PATH"].split(":")[0] == f"{prefix}/lib/pkgconfig", name
        assert any(call[0] == "install" for call in calls), name
    print(f"PASS {name}: {'rejected' if selected is None else selected}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script", type=Path, default=DEFAULT_SCRIPT,
                        help="installer under test (default: adjacent repository script)")
    script = parser.parse_args().script.resolve()
    if not script.is_file():
        parser.error(f"installer not found: {script}")

    skew = {"helper": True, "helper_formula": "openssl@3", "upgrade_to": "openssl@4",
            "openssl_installed": ["openssl@3"]}
    check(script, "old keg 3 -> installed helper 4", skew, "openssl@4")
    check(script, "stable helper 3", {"helper": True, "helper_formula": "openssl@3",
                                       "openssl_installed": ["openssl@3"]}, "openssl@3")
    check(script, "stable helper 4", {"helper": True, "helper_formula": "openssl@4",
                                       "openssl_installed": ["openssl@4"]}, "openssl@4")
    check(script, "requested helper missing", {"helper": False, "helper_formula": "openssl@4",
                                                "helper_after_install": False}, None)
    check(script, "ambiguous majors", {"helper": True, "helper_formula": "openssl@4",
                                       "deps_override": ["openssl@3", "openssl@4"],
                                       "openssl_installed": ["openssl@3", "openssl@4"]}, None)
    check(script, "brew deps error with partial output",
          {"helper": True, "helper_formula": "openssl@3", "deps_error": True,
           "openssl_installed": ["openssl@3"]}, None)
    check(script, "brew install error", {"helper": True, "helper_formula": "openssl@3",
                                         "install_error": True,
                                         "openssl_installed": ["openssl@3"]}, None)
    check(script, "QUIC auto can see existing helper without --quic",
          {"helper": True, "helper_formula": "openssl@3",
           "openssl_installed": ["openssl@3"]}, "openssl@3", profile="format-check")
    check(script, "no helper, no QUIC requested", {"helper": False, "helper_formula": "openssl@4"},
          "openssl", profile="format-check")


if __name__ == "__main__":
    main()
