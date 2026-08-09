#!/usr/bin/env bash

# Scaffold a qbm module from the published template.
#
# The clone URL here was WRONG: it named `isndev/qbm-sample`, which does not exist, while the
# `cd` two steps later named `qb-sample-module`, which does. Verified with `gh repo view` on
# both. The mismatch survived because the two were separate string literals; TEMPLATE below is
# now the single source for the clone URL and every message, so the two cannot drift again.
#
# The 404 was also not a clean failure. Without `set -e` the clone failed, the `cd` failed, and
# `mkdir .git; mv * .git` then ran against the user's INVOCATION directory and swallowed its
# contents -- exiting 0. Reproduced end to end before this was written. Hence the guards below;
# see qb-new-project.sh, which had the identical structure and the identical exposure.
#
# THREE MORE DEFECTS, all of the same shape -- exit 0, wrong result -- were measured after
# `set -e` landed, and are fixed below:
#
#  1. `git --bare init` takes HEAD from the user's `init.defaultBranch`, while the push named
#     `master` because that is the template's branch. On any machine configured with
#     `init.defaultBranch=main` -- increasingly the default -- HEAD pointed at a branch that
#     did not exist, `git reset --hard` succeeded against an unborn HEAD without writing a
#     single file, `git submodule update` found no `.gitmodules`, and the script printed
#     "Created 'mymodule'" and exited 0 over a directory containing nothing but `.git`.
#     Reproduced. `set -e` cannot see this: no command failed.
#     Nothing assumes a branch any more -- `git clone` takes HEAD from the source -- and the
#     result is ASSERTED to be non-empty before we claim success. That last check is the one
#     that turns this whole class loud.
#
#  2. `rm -rf "../${TEMPLATE}"` ran from inside `${NAME}`, so its meaning depended on where
#     `${NAME}` had put us. A name containing a slash or `..` (`qb-new-module.sh ../foo`)
#     moved the cleanup one directory up, where it could delete an unrelated
#     `qb-sample-module`. NAME is now validated as a plain directory name and every path is
#     resolved once, absolutely, from the invocation directory -- no destructive path is ever
#     computed from user input.
#
#  3. Nothing removed the half-built `${NAME}`/`${TEMPLATE}` directories when a later step
#     failed, so the guards above then refused the retry the user obviously wanted. A trap
#     now removes the one directory this script creates, and only on failure.
#
# `mkdir .git; mv * .git` is gone with them, and so is the bare-repo dance around it: a plain
# `git clone` into the target followed by `git remote remove origin` reaches the same result
# (full template history, no upstream remote) in two commands that cannot run anywhere else.
# Nothing is created outside `${NAME}` any more, so there is no intermediate `${TEMPLATE}`
# directory to collide with a user's, and no branch name is assumed anywhere -- clone takes
# HEAD from the source, which is what made `init.defaultBranch` irrelevant instead of fatal.
#
# The two intermediate forms tried before this one are worth recording, because each looked
# right and was not. `git --bare init` + push + convert is defect 1. `git init` + `git fetch
# +refs/heads/X:refs/heads/X` inverts it: fetch REFUSES to write the branch HEAD is on, so it
# failed whenever `init.defaultBranch` happened to MATCH the template's branch -- the common
# case -- and succeeded only when it differed. Both were caught by running the script under
# both `init.defaultBranch` settings; a single-configuration test passes either one of them.
#
# VERSION COUPLING -- read this before assuming the output is current. This script is fetched
# by URL from one branch of qb, and the template is cloned from ITS default branch; nothing in
# this file can make the two agree. The module template has not been touched since 2019 and
# does not configure against a current qb, so QB_TEMPLATE_REF exists for the day it carries a
# branch or tag that matches a qb release (git clone --branch takes either, not a sha):
#     QB_TEMPLATE_REF=v3.0.0 qb-new-module.sh mymodule
set -euo pipefail

TEMPLATE=qb-sample-module
TEMPLATE_URL="https://github.com/isndev/${TEMPLATE}.git"
TEMPLATE_REF="${QB_TEMPLATE_REF:-}"

# Exit if name argument is not given
if [ -z "$*" ]; then
    echo "A module name argument must be provided." >&2
    exit 2
fi

NAME=$1


################################################################################


# A module name is a directory name, not a path. Rejecting the rest is what keeps every
# path below a direct child of the invocation directory -- see defect 2 in the header.
if [ -z "${NAME}" ]; then
    echo "The module name must not be empty." >&2
    exit 2
fi
case "${NAME}" in
    */*|*\\*)
        echo "'${NAME}' is a path, not a module name -- pass a plain directory name." >&2
        exit 2
        ;;
    -*)
        echo "'${NAME}' starts with '-'; git would read it as an option." >&2
        exit 2
        ;;
    .|..)
        echo "'${NAME}' is not a usable module name." >&2
        exit 2
        ;;
esac

# Resolve the target ONCE, absolutely, from the invocation directory. NAME is validated above
# to contain no separator, so this is always a direct child and nothing this script does can
# reach outside it.
TARGET_DIR="${PWD}/${NAME}"

# Refuse to touch anything that is already there rather than merging into it.
if [ -e "${TARGET_DIR}" ]; then
    echo "'${NAME}' already exists here -- refusing to overwrite it." >&2
    exit 1
fi

# Remove ONLY the directory this script created, and only if it did not finish. Leaving the
# debris behind is not neutral: the guard above would then refuse the retry the user wants.
created_target=0
cleanup() {
    status=$?
    if [ "${status}" -ne 0 ]; then
        if [ "${created_target}" -eq 1 ]; then
            rm -rf "${TARGET_DIR}"
        fi
        echo "Failed to create '${NAME}'; nothing left behind." >&2
    fi
}
trap cleanup EXIT

# Clone the template straight into the target: git takes HEAD from the source, so no branch
# name is assumed and `init.defaultBranch` never enters into it.
created_target=1
if [ -n "${TEMPLATE_REF}" ]; then
    git clone --branch "${TEMPLATE_REF}" "${TEMPLATE_URL}" "${TARGET_DIR}"
else
    git clone "${TEMPLATE_URL}" "${TARGET_DIR}"
fi

# Detach the result from the template. `git remote remove` also drops refs/remotes/origin/*
# and the branch.<name>.remote config, so what is left is the template's history under the
# user's own name with nothing pointing back at isndev.
git -C "${TARGET_DIR}" remote remove origin

# `--branch <tag>` leaves a detached HEAD; put the user on a branch they can commit to.
BRANCH=$(git -C "${TARGET_DIR}" rev-parse --abbrev-ref HEAD)
if [ "${BRANCH}" = "HEAD" ]; then
    BRANCH=main
    git -C "${TARGET_DIR}" checkout --quiet -B "${BRANCH}"
fi

# ANTI-VACUITY. Everything above can succeed and still leave an empty tree -- that is exactly
# how defect 1 shipped, and it is the reason this script prints a file count rather than an
# unconditional "Created". Counting tracked files asserts the checkout happened without
# hard-coding what the template contains, which would rot the next time the template changes.
tracked=$(git -C "${TARGET_DIR}" ls-files | wc -l | tr -d '[:space:]')
if [ "${tracked}" -eq 0 ]; then
    echo "'${NAME}' came out empty: ${TEMPLATE}@${BRANCH} checked out 0 files." >&2
    echo "That is a bug in this script or in the template -- please report it." >&2
    exit 1
fi

git -C "${TARGET_DIR}" submodule update --init --recursive

echo "Created '${NAME}' from ${TEMPLATE_URL} (${BRANCH}), ${tracked} files."
echo "  A qbm module cannot be configured on its own: it calls qb_register_module()/qb_add_test(),"
echo "  which an installed qb does not ship. Build it from a root that add_subdirectory()es a qb"
echo "  SOURCE tree first -- your project's own root, or .github/ci/superbuild/CMakeLists.txt as"
echo "  used by qbm-http, qbm-pgsql and qbm-redis."
