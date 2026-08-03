#!/usr/bin/env bash
#
# Local clang-tidy runner for standalone qb.
#
# qb intentionally builds several implementation files through unity-style
# translation units that #include .cpp files. This script runs clang-tidy on the
# real compile_commands.json translation units, while mapping changed included
# .cpp/header files back to the TU(s) that compile them.
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}" || exit 2

BUILD_DIR="${QB_CLANG_TIDY_BUILD_DIR:-build/clang-tidy}"
CXX_STANDARD="${QB_CXX_STANDARD:-20}"
MODE="changed"
CONFIGURE=auto
QUIET=0
RAW_FILES=()

usage() {
    cat <<'EOF'
clang-tidy.sh - local clang-tidy runner for standalone qb.

Usage:
  ./scripts/clang-tidy.sh                 # changed qb files
  ./scripts/clang-tidy.sh --all           # all qb TUs from compile_commands.json
  ./scripts/clang-tidy.sh path/to/file    # explicit qb source/header files

Environment:
  CLANG_TIDY=/path/to/clang-tidy          # override tool lookup
  QB_CXX_STANDARD=20|23                   # default: 20
  QB_CLANG_TIDY_BUILD_DIR=build/foo       # default: build/clang-tidy
  QB_CLANG_TIDY_BASE_REF=HEAD             # default diff base for --changed

Options:
  --all             Run on all qb translation units, excluding vendor/modules/tests.
  --changed         Run on changed qb files mapped to their owning TU(s).
  --configure       Force-regenerate the clang-tidy build directory first.
  --no-configure    Do not configure; require an existing compile database.
  --quiet           Pass --quiet to clang-tidy.
  -h, --help        Show this help.
EOF
}

die() {
    printf 'clang-tidy: %s\n' "$*" >&2
    exit 1
}

find_clang_tidy() {
    if [ -n "${CLANG_TIDY:-}" ]; then
        [ -x "${CLANG_TIDY}" ] || die "CLANG_TIDY is set but not executable: ${CLANG_TIDY}"
        printf '%s\n' "${CLANG_TIDY}"
        return
    fi

    for candidate in \
        clang-tidy-23 clang-tidy-22 clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy \
        /opt/homebrew/opt/llvm/bin/clang-tidy \
        /usr/local/opt/llvm/bin/clang-tidy; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            command -v "${candidate}"
            return
        fi
        if [ -x "${candidate}" ]; then
            printf '%s\n' "${candidate}"
            return
        fi
    done

    die "clang-tidy not found. Install LLVM or set CLANG_TIDY=/path/to/clang-tidy."
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --all)
            MODE=all
            shift
            ;;
        --changed)
            MODE=changed
            shift
            ;;
        --configure)
            CONFIGURE=always
            shift
            ;;
        --no-configure)
            CONFIGURE=never
            shift
            ;;
        --quiet)
            QUIET=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                RAW_FILES+=("$1")
                shift
            done
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            RAW_FILES+=("$1")
            shift
            ;;
    esac
done

CLANG_TIDY_BIN="$(find_clang_tidy)"

if [ "${CONFIGURE}" = always ] || { [ "${CONFIGURE}" = auto ] && [ ! -f "${BUILD_DIR}/compile_commands.json" ]; }; then
    cmake -S . -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DQB_CXX_STANDARD="${CXX_STANDARD}" \
        -DQB_BUILD_TESTS=OFF \
        -DQB_BUILD_EXAMPLES=OFF \
        -DQB_BUILD_BENCHMARKS=OFF \
        -DQB_ENABLE_NATIVE_ARCH=OFF
elif [ "${CONFIGURE}" = never ] && [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
    die "missing ${BUILD_DIR}/compile_commands.json; rerun without --no-configure"
fi

[ -f "${BUILD_DIR}/compile_commands.json" ] || die "missing ${BUILD_DIR}/compile_commands.json"

PLAN_INPUT=()
if [ "${#RAW_FILES[@]}" -gt 0 ]; then
    PLAN_INPUT=("${RAW_FILES[@]}")
elif [ "${MODE}" = changed ]; then
    base_ref="${QB_CLANG_TIDY_BASE_REF:-HEAD}"
    while IFS= read -r file; do
        PLAN_INPUT+=("${file}")
    done < <(
        {
            git diff --name-only --diff-filter=ACMRT "${base_ref}" -- \
                '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' 2>/dev/null
            git diff --name-only --diff-filter=ACMRT --cached -- \
                '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' 2>/dev/null
        } | sort -u || true
    )
fi

PLAN_OUTPUT="$(
    python3 - "${ROOT}" "${BUILD_DIR}/compile_commands.json" "${MODE}" "${#PLAN_INPUT[@]}" "${PLAN_INPUT[@]}" <<'PY'
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

root = Path(sys.argv[1]).resolve()
compile_db = Path(sys.argv[2])
mode = sys.argv[3]
inputs = [Path(p).as_posix().removeprefix("./") for p in sys.argv[5:]]

code_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
tu_suffixes = {".c", ".cc", ".cpp", ".cxx"}
include_re = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

def relpath(path: Path):
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return None

def in_scope(rel: str) -> bool:
    if not rel:
        return False
    parts = rel.split("/")
    if "build" in parts or "modules" in parts or "vendor" in parts or "tests" in parts:
        return False
    if Path(rel).suffix not in code_suffixes:
        return False
    return rel.startswith("src/qb/")

def resolve_include(owner: str, delimiter: str, target: str, tracked: set[str]):
    candidates = []
    if delimiter == '"':
        candidates.append((root / owner).parent / target)
    if target.startswith("qb/"):
        candidates.append(root / "src" / target)
    candidates.append(root / target)

    for candidate in candidates:
        rel = relpath(candidate)
        if rel in tracked:
            return rel
    return None

tracked = {
    Path(path).as_posix()
    for path in subprocess.check_output(
        ["git", "ls-files"], cwd=root, text=True
    ).splitlines()
    if in_scope(Path(path).as_posix())
}

project_tus = []
for entry in json.loads(compile_db.read_text()):
    rel = relpath(Path(entry["file"]))
    if rel and in_scope(rel) and Path(rel).suffix in tu_suffixes:
        if rel not in project_tus:
            project_tus.append(rel)

if not project_tus:
    raise SystemExit("STAT error no qb translation units found")

includes = defaultdict(list)
for rel in tracked:
    path = root / rel
    try:
        text = path.read_text(errors="ignore").splitlines()
    except OSError:
        continue
    for line in text:
        match = include_re.match(line)
        if not match:
            continue
        resolved = resolve_include(rel, match.group(1), match.group(2), tracked)
        if resolved:
            includes[rel].append(resolved)

owners = defaultdict(set)
for tu in project_tus:
    seen = set()
    stack = [tu]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        owners[current].add(tu)
        stack.extend(includes.get(current, ()))

def select_for(rel: str):
    if rel in project_tus:
        return {rel}
    if rel in owners:
        return set(owners[rel])
    if rel.startswith("src/qb/"):
        return set(project_tus)
    return set()

selected = []
skipped = []
input_count = 0

if mode == "all" and not inputs:
    selected = list(project_tus)
    input_count = len(tracked)
else:
    selected_set = set()
    for raw in inputs:
        rel = Path(raw).as_posix().removeprefix("./")
        if not in_scope(rel):
            skipped.append(rel)
            continue
        input_count += 1
        selected_set.update(select_for(rel))
    selected = [tu for tu in project_tus if tu in selected_set]

print(f"STAT scope_code_files {len(tracked)}")
print(f"STAT project_translation_units {len(project_tus)}")
print(f"STAT input_files_in_scope {input_count}")
for rel in skipped:
    print(f"SKIP {rel}")
for tu in selected:
    print(f"TU {tu}")
PY
)"

SCOPE_CODE_FILES=0
PROJECT_TUS=0
INPUT_FILES=0
FILES=()
while IFS= read -r line; do
    case "${line}" in
        "STAT scope_code_files "*)
            SCOPE_CODE_FILES="${line##* }"
            ;;
        "STAT project_translation_units "*)
            PROJECT_TUS="${line##* }"
            ;;
        "STAT input_files_in_scope "*)
            INPUT_FILES="${line##* }"
            ;;
        "SKIP "*)
            printf 'clang-tidy: skip out-of-scope file: %s\n' "${line#SKIP }" >&2
            ;;
        "TU "*)
            FILES+=("${line#TU }")
            ;;
    esac
done <<< "${PLAN_OUTPUT}"

if [ "${#FILES[@]}" -eq 0 ]; then
    printf 'clang-tidy: no C++ translation units selected\n'
    exit 0
fi

TIDY_ARGS=(-p "${BUILD_DIR}")
[ "${QUIET}" -eq 1 ] && TIDY_ARGS+=(--quiet)

EXTRA_ARGS=()
if [ "$(uname -s)" = Darwin ] && command -v xcrun >/dev/null 2>&1; then
    SDKROOT="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [ -n "${SDKROOT}" ]; then
        EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="${SDKROOT}")
    fi
fi

printf 'clang-tidy: %s\n' "$("${CLANG_TIDY_BIN}" --version | head -n 1)"
printf 'clang-tidy: build dir: %s\n' "${BUILD_DIR}"
printf 'clang-tidy: project code files in scope: %s\n' "${SCOPE_CODE_FILES}"
printf 'clang-tidy: project translation units: %s\n' "${PROJECT_TUS}"
printf 'clang-tidy: input files in scope: %s\n' "${INPUT_FILES}"
printf 'clang-tidy: selected translation units: %s\n' "${#FILES[@]}"

status=0
for file in "${FILES[@]}"; do
    if [ ! -f "${file}" ]; then
        printf 'clang-tidy: skip missing file: %s\n' "${file}" >&2
        continue
    fi
    printf '\n==> %s\n' "${file}"
    "${CLANG_TIDY_BIN}" "${TIDY_ARGS[@]}" "${EXTRA_ARGS[@]}" "${file}" || status=$?
done

exit "${status}"
