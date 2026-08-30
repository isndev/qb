#!/usr/bin/env bash

# Scaffold a qbm module from the published template.
#
# Documented as a curl-pipe-to-bash one-liner (qb/README.md), which means it runs in a directory
# the user cares about and whose contents it did not create. That is why every step is guarded,
# and why the network work now happens in a scratch directory outside it.
#
# ---------------------------------------------------------------------------------------------
# WHAT THIS SCRIPT PRODUCES, AND WHY THAT CHANGED
# ---------------------------------------------------------------------------------------------
# It produces a NEW repository: the template's payload rendered under the user's module name,
# committed once, with no remote. It used to `git clone` the template and then
# `git remote remove origin`, which handed the user somebody else's commit history as their own.
# Three structural defects were fixed together, and each had shipped:
#
#  1. HISTORY. See above. Fixed by cloning into a scratch directory, copying the payload subtree,
#     and running `git init` + one commit in the target. A side effect worth stating: this
#     retires the last reason for this script to construct a `.git` directory by hand, which is
#     the construct that once moved a user's files into one.
#
#  2. IDENTIFIERS. This template was the worse of the two: it shipped `project(sample)`, a
#     `qbm::sample` namespace, `QB_MODULE_SAMPLE_*` include guards and `actor/sample.h`, so
#     `qb-new-module.sh mymodule` produced a directory called `mymodule` containing a module
#     called `sample`. Since 3.0 that is not merely untidy -- qb_register_module() requires the
#     module's headers at src/qbm/<NAME>/, so renaming means moving directories, and the tree
#     does not configure until you do. The template now carries @QB_NAME@ / @QB_NAME_LOWER@ /
#     @QB_NAME_UPPER@ placeholders in file contents AND in path names, and this script
#     substitutes all of them from the ONE name the user passed. The vocabulary is owned here,
#     not by the template, and an unresolved placeholder is a hard error.
#
#  3. VERSION BINDING. Nothing bound a template version to a qb version, so they drifted silently
#     and independently -- this template had not been touched since 2019 and did not configure at
#     all. The generated tree now fetches qb with FetchContent at a ref THIS SCRIPT WRITES,
#     resolved from QB_SHIPPED_VERSION below, which is asserted equal to qbConfig.cmake's
#     QB_FRAMEWORK_VERSION by scripts/check-scaffold-consistency.sh. The URL the one-liner is
#     fetched from selects the pairing.
#
# A qbm module still CANNOT be configured on its own -- it calls qb_register_module() and
# qb_add_test(), development-time helpers an installed qb does not ship -- so the generated tree
# carries the same answer the three real modules carry: a minimal superbuild root at
# .github/ci/superbuild/CMakeLists.txt that add_subdirectory()s a qb SOURCE tree first and the
# module second. The generated copy differs from qbm-http/pgsql/redis's in exactly one way, and
# deliberately: theirs require both trees to be passed in, because their CI always passes them,
# while a freshly scaffolded module defaults its own path and fetches qb, so
# `cmake -S .github/ci/superbuild -B build` works with no arguments on a machine with no qb.
#
# ---------------------------------------------------------------------------------------------
# THE FOUR EARLIER DEFECTS, all of the same shape -- exit 0, wrong result -- kept fixed
# ---------------------------------------------------------------------------------------------
#  * The clone URL named `isndev/qbm-sample`, which does not exist, while the `cd` two steps later
#    named `qb-sample-module`, which does; the two were separate string literals and had drifted.
#    TEMPLATE below is the single source for the URL and every message.
#  * That 404 was not a clean failure: without `set -e` the clone failed, the `cd` failed, and
#    `mkdir .git; mv * .git` ran against the user's INVOCATION directory and swallowed its
#    contents, exiting 0. Reproduced end to end before it was fixed.
#  * `git --bare init` took HEAD from the user's `init.defaultBranch` while the push named the
#    template's branch, so the script could print "Created" over a directory containing nothing
#    but `.git`. `set -e` cannot see this: no command failed. Hence the anti-vacuity checks.
#  * `rm -rf "../${TEMPLATE}"` was computed from a user-supplied name, and nothing removed the
#    half-built target on failure. Every path is now resolved once, absolutely, and a trap removes
#    only what this script created, only on failure.
#
# All of them are exercised by qb's .github/workflows/scaffold.yml, which runs this script under
# both `init.defaultBranch` settings and plants each defect shape as a negative control.
set -euo pipefail

# ============================== per-kind configuration ==============================
# Everything below the SHARED BODY marker is byte-identical to qb-new-project.sh. This block is
# the only part that differs, and scripts/check-scaffold-consistency.sh checks exactly that.

KIND=module
TEMPLATE=qb-sample-module

# Stricter than the project's, and for a reason that is not style: this name becomes the
# `qbm-<name>` target, the `qbm::<name>` alias, the `qbm::<name>` C++ namespace and the
# src/qbm/<name>/ directory that qb_register_module() requires. The three real modules are
# `http`, `pgsql` and `redis`; anything that is not a lowercase identifier would produce a tree
# that does not compile, so it is refused here rather than at the first error.
NAME_PATTERN='^[a-z][a-z0-9_]*$'
NAME_RULE="start with a lowercase letter and contain only lowercase letters, digits or '_'"

# The qb version this copy of the script ships with. NOT a hand-maintained constant: it is
# asserted equal to QB_FRAMEWORK_VERSION in qb/cmake/qbConfig.cmake by
# scripts/check-scaffold-consistency.sh, which runs in scaffold.yml and in the superproject's
# verify.sh. Bumping the framework version without bumping this is a red build.
QB_SHIPPED_VERSION=3.1.0

next_steps() {
    cat <<EOF
Next steps:

    cd ${NAME}
    cmake -S .github/ci/superbuild -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel --target qbm-${NAME}-tests
    ctest --test-dir build --output-on-failure -L module:qbm-${NAME}

Name the target and the label. QB_BUILD_TESTS is global, so the same build also registers qb's
own ~174 tests; an unqualified build compiles all of them and an unqualified ctest tries to run
them. Both commands above are scoped to this module.

A qbm module cannot be configured on its own: it calls qb_register_module()/qb_add_test(), which
an installed qb does not ship. That superbuild root is the entry point -- it add_subdirectory()s a
qb SOURCE tree (fetched at ref ${QB_BUILD_REF}) and then this module. It is the same shape
qbm-http, qbm-pgsql and qbm-redis use.

To build against a qb checkout you already have:

    cmake -S .github/ci/superbuild -B build -DQBM_CI_QB_DIR=/path/to/qb

To use this module from a project, add it to that project's qbm/ directory -- the project
template's qb_load_modules() call picks it up and exposes it as qbm::${NAME}.
EOF
}
# ============================ end per-kind configuration ============================

# >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> SHARED BODY >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
# Everything below this marker is BYTE-IDENTICAL in qb-new-project.sh and qb-new-module.sh,
# and scripts/check-scaffold-consistency.sh fails the build if the two copies drift. The two
# scripts are separate files because each is fetched on its own by `curl | bash`: a shared
# implementation would mean a second network fetch, a second thing to read before running, and
# a version skew between the two fetches. One reviewed body copied twice, with a guard, is the
# same trade this tree already makes for scripts/llm-guard.py (four copies) and
# .github/ci/superbuild/CMakeLists.txt (three copies).
#
# Do not edit one copy. Edit the body, then re-run scripts/check-scaffold-consistency.sh.
# ------------------------------------------------------------------------------------------

TEMPLATE_URL="https://github.com/isndev/${TEMPLATE}.git"
QB_URL="https://github.com/isndev/qb.git"

# The subdirectory of the template repository that BECOMES the user's tree. Everything outside
# it -- the template's own README, LICENSE and CI -- describes the template and must not be
# copied into a scaffolded project, which is why the payload is a named directory rather than
# the repository root with an exclusion list. An exclusion list is a thing you forget to update.
PAYLOAD_SUBDIR=template

# A scaffolder must never sit waiting on a credential prompt: `curl | bash` gives it the user's
# terminal, and every network operation below is against a public repository.
export GIT_TERMINAL_PROMPT=0

usage() {
    cat >&2 <<EOF
usage: qb-new-${KIND}.sh <${KIND}-name>

Creates ./<${KIND}-name> from the ${TEMPLATE} template as a FRESH git repository
with a single initial commit -- not a copy of the template's history.

Environment overrides (all optional):
  QB_TEMPLATE_REF=<ref>   template branch or tag to use  (default: resolved from the qb version)
  QB_TEMPLATE_DIR=<path>  use a local template checkout instead of cloning (for template authors)
  QB_REF=<ref>            the isndev/qb ref the generated tree builds against
EOF
}

# ---------------------------------------------------------------------------- the name

if [ -z "${1:-}" ]; then
    echo "A ${KIND} name argument must be provided." >&2
    usage
    exit 2
fi

NAME=$1

# A name is a directory name, not a path. Rejecting the rest is what keeps every path below a
# direct child of the invocation directory: no destructive path is ever computed from user input.
case "${NAME}" in
    */*|*\\*)
        echo "'${NAME}' is a path, not a ${KIND} name -- pass a plain directory name." >&2
        exit 2
        ;;
    -*)
        echo "'${NAME}' starts with '-'; git would read it as an option." >&2
        exit 2
        ;;
    .|..)
        echo "'${NAME}' is not a usable ${KIND} name." >&2
        exit 2
        ;;
esac

# The name is not just a directory: it is substituted into CMake target names, C++ namespaces,
# include guards and file paths. A name that is a fine directory name and a bad identifier would
# produce a tree that does not compile, which is the failure this scaffolder exists to prevent.
if ! printf '%s' "${NAME}" | grep -Eq "${NAME_PATTERN}"; then
    echo "'${NAME}' cannot be used as a ${KIND} name: it must ${NAME_RULE}." >&2
    echo "The name becomes CMake targets, C++ identifiers and directory names, so it is" >&2
    echo "checked here rather than at the first compile error." >&2
    exit 2
fi

# Both refs are spliced into generated CMake and into git command lines. Neither comes from the
# template, but both come from the environment, so both are validated to a conservative ref
# charset before they are used anywhere.
for _ref_var in QB_TEMPLATE_REF QB_REF; do
    eval "_ref_val=\${${_ref_var}:-}"
    if [ -n "${_ref_val}" ] && ! printf '%s' "${_ref_val}" | grep -Eq '^[A-Za-z0-9._/-]+$'; then
        echo "${_ref_var}='${_ref_val}' is not a plausible git ref." >&2
        exit 2
    fi
done

# Resolve the target ONCE, absolutely, from the invocation directory. NAME is validated above to
# contain no separator, so this is always a direct child and nothing this script does can reach
# outside it.
TARGET_DIR="${PWD}/${NAME}"

if [ -e "${TARGET_DIR}" ]; then
    echo "'${NAME}' already exists here -- refusing to overwrite it." >&2
    exit 1
fi

# ---------------------------------------------------------------------------- cleanup

# The scratch directory is OUTSIDE the invocation directory, which is the structural reason the
# old data-loss defect cannot recur: the clone no longer happens anywhere near the user's files.
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/qb-new-XXXXXX")
created_target=0

cleanup() {
    _status=$?
    if [ -n "${WORK_DIR}" ]; then
        rm -rf "${WORK_DIR}"
    fi
    if [ "${_status}" -ne 0 ]; then
        if [ "${created_target}" -eq 1 ]; then
            rm -rf "${TARGET_DIR}"
        fi
        echo "Failed to create '${NAME}'; nothing left behind." >&2
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------- ref resolution

# Three outcomes, kept distinct on purpose. `git ls-remote --exit-code` exits 0 when the ref is
# there, 2 when the remote answered and does not have it, and something else (128) when the probe
# itself failed. Collapsing "definitely absent" into "could not tell" is what would turn a network
# blip into a silently wrong pairing, so this returns all three and every caller handles all three.
remote_ref_state() {
    _rc=0
    git ls-remote --exit-code --heads --tags "$1" "$2" >/dev/null 2>&1 || _rc=$?
    case "${_rc}" in
        0) return 0 ;;
        2) return 2 ;;
        *) return 1 ;;
    esac
}

QB_VERSION_TAG="v${QB_SHIPPED_VERSION}"

# WHICH qb DOES THE GENERATED TREE BUILD AGAINST?
#
# This is the question the old templates got wrong for two major versions, and the reason they
# got it wrong is that the answer was STORED -- a submodule gitlink committed in the template,
# which can only ever drift away from the qb whose script cloned it. Nothing stores it now: the
# ref is decided here, at scaffold time, from the version of the qb this script SHIPS WITH.
# QB_SHIPPED_VERSION is set in this file's configuration block and is asserted equal to
# qb/cmake/qbConfig.cmake's QB_FRAMEWORK_VERSION by scripts/check-scaffold-consistency.sh, so
# fetching this script from a branch or a tag selects the pairing:
#     .../qb/main/script/...    -> the released line
#     .../qb/v3.0.0/script/...  -> exactly that release
#
# Whether v<version> EXISTS is also the signal for which template branch to use, so the probe
# runs once and both decisions read it.
QB_RELEASE_STATE=unknown
if [ -n "${QB_REF:-}" ]; then
    QB_BUILD_REF="${QB_REF}"
    QB_REF_WHY="QB_REF was set explicitly"
else
    _rc=0
    remote_ref_state "${QB_URL}" "${QB_VERSION_TAG}" || _rc=$?
    case "${_rc}" in
        0) QB_RELEASE_STATE=released ;;
        2) QB_RELEASE_STATE=unreleased ;;
        *) QB_RELEASE_STATE=unknown ;;
    esac
    case "${QB_RELEASE_STATE}" in
        released)
            QB_BUILD_REF="${QB_VERSION_TAG}"
            QB_REF_WHY="qb ${QB_SHIPPED_VERSION} is released, so the generated tree pins ${QB_VERSION_TAG}"
            ;;
        unreleased)
            QB_BUILD_REF=develop
            QB_REF_WHY="qb ${QB_SHIPPED_VERSION} is not tagged yet (no ${QB_VERSION_TAG} on ${QB_URL}), so the generated tree follows the 'develop' branch where ${QB_SHIPPED_VERSION} lives"
            ;;
        *)
            QB_BUILD_REF="${QB_VERSION_TAG}"
            QB_REF_WHY="could not reach ${QB_URL} to check for ${QB_VERSION_TAG}; assuming it exists"
            ;;
    esac
fi

# WHICH TEMPLATE?
#
# Same source of truth, same probe. An explicit QB_TEMPLATE_REF always wins. Otherwise the
# ladder is: the release pairing (v<version>), then -- only when qb <version> is KNOWN to be
# unreleased -- the development line, then the template's default branch. The last rung is a
# fallback and is reported as one: a scaffolder that silently hands you a template from a
# different era is exactly how this rotted the first time.
TEMPLATE_SOURCE=
TEMPLATE_REF=
if [ -n "${QB_TEMPLATE_DIR:-}" ]; then
    if [ ! -d "${QB_TEMPLATE_DIR}" ]; then
        echo "QB_TEMPLATE_DIR='${QB_TEMPLATE_DIR}' is not a directory." >&2
        exit 2
    fi
    TEMPLATE_SOURCE="${QB_TEMPLATE_DIR}"
    TEMPLATE_REF="(local)"
    TEMPLATE_REF_WHY="QB_TEMPLATE_DIR was set -- nothing was cloned"
elif [ -n "${QB_TEMPLATE_REF:-}" ]; then
    TEMPLATE_REF="${QB_TEMPLATE_REF}"
    TEMPLATE_REF_WHY="QB_TEMPLATE_REF was set explicitly"
else
    _candidates="${QB_VERSION_TAG}"
    if [ "${QB_RELEASE_STATE}" = unreleased ]; then
        _candidates="${_candidates} develop"
    fi
    for _candidate in ${_candidates}; do
        _rc=0
        remote_ref_state "${TEMPLATE_URL}" "${_candidate}" || _rc=$?
        if [ "${_rc}" -eq 0 ]; then
            TEMPLATE_REF="${_candidate}"
            break
        fi
    done
    if [ -n "${TEMPLATE_REF}" ]; then
        if [ "${TEMPLATE_REF}" = "${QB_VERSION_TAG}" ]; then
            TEMPLATE_REF_WHY="matches qb ${QB_SHIPPED_VERSION}"
        else
            TEMPLATE_REF_WHY="qb ${QB_SHIPPED_VERSION} is not released yet and ${TEMPLATE} has no ${QB_VERSION_TAG}, so the development line is the matching one"
        fi
    else
        TEMPLATE_REF_WHY="FALLBACK -- ${TEMPLATE} has neither ${QB_VERSION_TAG} nor a matching development branch, so its default branch was used and may not match qb ${QB_SHIPPED_VERSION}"
    fi
fi

# ---------------------------------------------------------------------------- fetch

if [ -z "${TEMPLATE_SOURCE}" ]; then
    TEMPLATE_SOURCE="${WORK_DIR}/template"
    if [ -n "${TEMPLATE_REF}" ]; then
        git clone --quiet --depth 1 --branch "${TEMPLATE_REF}" "${TEMPLATE_URL}" "${TEMPLATE_SOURCE}"
    else
        git clone --quiet --depth 1 "${TEMPLATE_URL}" "${TEMPLATE_SOURCE}"
        TEMPLATE_REF=$(git -C "${TEMPLATE_SOURCE}" rev-parse --abbrev-ref HEAD)
    fi
fi

PAYLOAD="${TEMPLATE_SOURCE}/${PAYLOAD_SUBDIR}"

# ANTI-VACUITY, first of four. Every command above can succeed and still leave nothing to copy --
# that is precisely how a previous version of this script printed "Created" over a directory
# containing only `.git`. Neither branch below is reachable with a healthy template, which is why
# .github/workflows/scaffold.yml drives both with a deliberately empty stand-in repository.
if [ ! -d "${PAYLOAD}" ]; then
    echo "'${NAME}' came out empty: ${TEMPLATE}@${TEMPLATE_REF} has no ${PAYLOAD_SUBDIR}/ directory." >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

payload_files=$(find "${PAYLOAD}" -type f | wc -l | tr -d '[:space:]')
if [ "${payload_files}" -eq 0 ]; then
    echo "'${NAME}' came out empty: ${TEMPLATE}@${TEMPLATE_REF} has an empty ${PAYLOAD_SUBDIR}/ directory." >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

# ---------------------------------------------------------------------------- copy

# `cp -R src/. dst/` copies the contents INCLUDING dotfiles on both BSD and GNU cp -- the
# payload carries .gitignore and .github/, and a plain `cp -R src dst` would nest or drop them.
mkdir -p "${TARGET_DIR}"
created_target=1
cp -R "${PAYLOAD}/." "${TARGET_DIR}/"

# ---------------------------------------------------------------------------- render

# One input, every casing derived from it -- the user names the thing once. NAME is already
# validated to [A-Za-z0-9_-], so `tr` here cannot produce anything surprising, and no value
# substituted below can contain a sed delimiter.
NAME_LOWER=$(printf '%s' "${NAME}" | tr '[:upper:]' '[:lower:]')
NAME_UPPER=$(printf '%s' "${NAME}" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')

render() {
    sed -e "s|@QB_NAME@|${NAME}|g" \
        -e "s|@QB_NAME_LOWER@|${NAME_LOWER}|g" \
        -e "s|@QB_NAME_UPPER@|${NAME_UPPER}|g" \
        -e "s|@QB_REF@|${QB_BUILD_REF}|g" \
        -e "s|@QB_VERSION@|${QB_SHIPPED_VERSION}|g" \
        -e "s|@QB_TEMPLATE_REF@|${TEMPLATE_REF}|g"
}

rendered_files=0
find "${TARGET_DIR}" -type f > "${WORK_DIR}/files.txt"
while IFS= read -r _f; do
    [ -s "${_f}" ] || continue
    # `grep -I` reports a binary file as non-matching, so this skips anything that is not text
    # rather than corrupting it. A template is text today; this keeps that from being load-bearing.
    grep -Iq . "${_f}" 2>/dev/null || continue
    if grep -q '@QB_[A-Z_]*@' "${_f}" 2>/dev/null; then
        render < "${_f}" > "${WORK_DIR}/rendered"
        # Write THROUGH the existing file rather than mv-ing over it: mv would replace the inode
        # and silently drop the executable bit from any script the template ships.
        cat "${WORK_DIR}/rendered" > "${_f}"
        rendered_files=$((rendered_files + 1))
    fi
done < "${WORK_DIR}/files.txt"

# Paths carry placeholders too (src/qbm/@QB_NAME@/...). `-depth` renames the deepest entry first,
# so renaming a directory can never invalidate a path still queued behind it.
renamed_paths=0
find "${TARGET_DIR}" -depth -name '*@QB_*@*' > "${WORK_DIR}/paths.txt"
while IFS= read -r _p; do
    [ -e "${_p}" ] || continue
    _dir=$(dirname "${_p}")
    _base=$(basename "${_p}")
    _new=$(printf '%s' "${_base}" | render)
    if [ "${_base}" != "${_new}" ]; then
        mv "${_p}" "${_dir}/${_new}"
        renamed_paths=$((renamed_paths + 1))
    fi
done < "${WORK_DIR}/paths.txt"

# ANTI-VACUITY, second: the tree must actually have been personalised. A template that lost its
# placeholders would copy cleanly, compile cleanly, and hand every user a project named after the
# template -- a wrong result with a zero exit, which is the whole class this script guards against.
if [ "${rendered_files}" -eq 0 ] && [ "${renamed_paths}" -eq 0 ]; then
    echo "'${NAME}' was not personalised: ${TEMPLATE}@${TEMPLATE_REF} contains no @QB_...@ placeholder." >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

# ANTI-VACUITY, third: nothing may be left unresolved. This is what catches a template that
# introduces a placeholder this script does not know about -- the substitution vocabulary is
# owned here, and a template cannot extend it silently.
leftover=$(grep -rIl '@QB_[A-Z_]*@' "${TARGET_DIR}" 2>/dev/null || true)
leftover_paths=$(find "${TARGET_DIR}" -name '*@QB_*@*' 2>/dev/null || true)
if [ -n "${leftover}" ] || [ -n "${leftover_paths}" ]; then
    echo "'${NAME}' has unresolved placeholders -- this script does not know them:" >&2
    [ -z "${leftover}" ] || printf '  in file: %s\n' ${leftover} >&2
    [ -z "${leftover_paths}" ] || printf '  in path: %s\n' ${leftover_paths} >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

# ANTI-VACUITY, fourth: the result must be a buildable shape, not merely a non-empty directory.
files=$(find "${TARGET_DIR}" -type f | wc -l | tr -d '[:space:]')
if [ "${files}" -eq 0 ] || [ ! -f "${TARGET_DIR}/CMakeLists.txt" ]; then
    echo "'${NAME}' came out empty: no CMakeLists.txt in the generated tree (${files} files)." >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

# ---------------------------------------------------------------------------- fresh history

# A NEW repository with ONE commit -- not the template's history under a new name. The old script
# cloned and then dropped the remote, which left the user's first `git log` showing years of
# somebody else's commits and their first `git push` pushing them. Every mainstream scaffolder
# (cargo new, npm init, dotnet new) starts you at one commit, and doing the same here also
# retires the last reason to touch `.git` by hand -- the construct behind the data-loss defect.
#
# The branch is whatever the user's own `init.defaultBranch` says: this is their repository, and
# a scaffolder that overrides that setting is answering a question it was not asked.
git init --quiet "${TARGET_DIR}"
git -C "${TARGET_DIR}" add -A

commit_made=0
# --no-verify because a globally configured hooksPath belongs to the user's OTHER projects; a
# synthetic initial commit must not be able to fail on somebody's commit-msg linter.
if git -C "${TARGET_DIR}" commit --quiet --no-verify \
       -m "Initial commit from ${TEMPLATE} (qb ${QB_SHIPPED_VERSION})" \
       > "${WORK_DIR}/commit.log" 2>&1; then
    commit_made=1
fi

BRANCH=$(git -C "${TARGET_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)

# THE RESULT IS NOW COMPLETE, so stop the trap from being able to delete it. Everything after
# this point only PRINTS, and nothing that merely prints should be able to destroy a correct
# tree -- but it could, and did: piping this script's stdout into `head` closes the pipe
# mid-report, the next `echo` fails with EPIPE, `set -e` exits non-zero, and the trap removed a
# finished project. Reproduced deterministically (`qb-new-project.sh X | head -8`) before this
# line was added; the target survives now and the exit status is still non-zero, which is the
# right pair. The trap keeps cleaning up genuine mid-flight failures, which is all it was for.
created_target=0

# ---------------------------------------------------------------------------- report

echo "Created '${NAME}' -- ${files} files, a fresh git repository, no upstream remote."
echo
echo "  template : ${TEMPLATE} @ ${TEMPLATE_REF}"
echo "             ${TEMPLATE_REF_WHY}"
echo "  qb ref   : ${QB_BUILD_REF}"
echo "             ${QB_REF_WHY}"
echo "  rendered : ${rendered_files} files, ${renamed_paths} paths renamed for '${NAME}'"
if [ "${commit_made}" -eq 1 ]; then
    echo "  git      : branch ${BRANCH}, commit $(git -C "${TARGET_DIR}" rev-parse --short HEAD)"
else
    # Not a failure of the scaffold: the tree is complete and staged. Said loudly rather than
    # swallowed, because "there is no commit" is not something to discover three days later.
    echo "  git      : branch ${BRANCH}, everything STAGED BUT NOT COMMITTED"
    echo
    echo "  git could not create the initial commit:" >&2
    sed 's/^/    /' "${WORK_DIR}/commit.log" >&2
    echo "  Set your identity and commit it yourself:" >&2
    echo "    git -C '${NAME}' config user.name  'Your Name'" >&2
    echo "    git -C '${NAME}' config user.email 'you@example.com'" >&2
    echo "    git -C '${NAME}' commit -m 'Initial commit'" >&2
fi
echo

next_steps
