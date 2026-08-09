#!/usr/bin/env bash

# Scaffold a qb project from the published template.
#
# This script is documented as a curl-pipe-to-bash one-liner (qb/README.md), which means it
# runs in a directory the user cares about and whose contents it did not create. That is why
# every step below is guarded.
#
# It used to have no `set -e`, and the consequence was not cosmetic: when `git clone` failed
# for ANY reason -- no network, a leftover template directory, or the repository simply not
# being there (which is exactly what qb-new-module.sh's sibling URL did) -- the `cd` on the
# next line failed too, `cd ../${NAME}` failed after it, and `mkdir .git; mv * .git` then ran
# against the INVOCATION directory, moving everything the user had there into a `.git` folder.
# The script exited 0, so a caller saw success. Reproduced end to end before this was written.
#
# TEMPLATE is one variable on purpose. The template name used to appear as three separate
# literals -- clone, cd, cleanup -- and they had already drifted apart: the module script
# cloned `isndev/qbm-sample` (which does not exist) and then cd'd into `qb-sample-module`
# (which does). One name cannot disagree with itself.
#
# THREE MORE DEFECTS, all of the same shape -- exit 0, wrong result -- were measured after
# `set -e` landed, and are fixed below:
#
#  1. `git --bare init` takes HEAD from the user's `init.defaultBranch`, while the push named
#     `master` because that is the template's branch. On any machine configured with
#     `init.defaultBranch=main` -- increasingly the default -- HEAD pointed at a branch that
#     did not exist, `git reset --hard` succeeded against an unborn HEAD without writing a
#     single file, `git submodule update` found no `.gitmodules`, and the script printed
#     "Created 'MyProject'" and exited 0 over a directory containing nothing but `.git`.
#     Reproduced. `set -e` cannot see this: no command failed.
#     Nothing assumes a branch any more -- `git clone` takes HEAD from the source -- and the
#     result is ASSERTED to be non-empty before we claim success. That last check is the one
#     that turns this whole class loud.
#
#  2. `rm -rf "../${TEMPLATE}"` ran from inside `${NAME}`, so its meaning depended on where
#     `${NAME}` had put us. A name containing a slash or `..` (`qb-new-project.sh ../foo`)
#     moved the cleanup one directory up, where it could delete an unrelated
#     `qb-sample-project`. NAME is now validated as a plain directory name and every path is
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
# by URL from one branch of qb, the template is cloned from ITS default branch, and the qb the
# template pins as a submodule is a third, independent thing. Nothing in this file can make
# those agree, and they do not: measured, the template pins a qb from before v2.0.0. So the
# script reports what it actually produced rather than claiming it is current, and takes
# QB_TEMPLATE_REF for the day the template carries a branch or tag matching a qb release
# (`git clone --branch` takes either, not a sha):
#     QB_TEMPLATE_REF=v3.0.0 qb-new-project.sh MyProject
set -euo pipefail

TEMPLATE=qb-sample-project
TEMPLATE_URL="https://github.com/isndev/${TEMPLATE}.git"
TEMPLATE_REF="${QB_TEMPLATE_REF:-}"

# Exit if name argument is not given
if [ -z "$*" ]; then
    echo "A project name argument must be provided." >&2
    exit 2
fi

NAME=$1


################################################################################


# A project name is a directory name, not a path. Rejecting the rest is what keeps every
# path below a direct child of the invocation directory -- see defect 2 in the header.
if [ -z "${NAME}" ]; then
    echo "The project name must not be empty." >&2
    exit 2
fi
case "${NAME}" in
    */*|*\\*)
        echo "'${NAME}' is a path, not a project name -- pass a plain directory name." >&2
        exit 2
        ;;
    -*)
        echo "'${NAME}' starts with '-'; git would read it as an option." >&2
        exit 2
        ;;
    .|..)
        echo "'${NAME}' is not a usable project name." >&2
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

# Report the qb the TEMPLATE pinned -- observed, not asserted. This script cannot make the
# template track a qb release, and the two are not in fact in step; saying which qb you got
# is the difference between finding that out now and finding it out from a compile error.
if [ -e "${TARGET_DIR}/qb/.git" ]; then
    qb_desc=$(git -C "${TARGET_DIR}/qb" describe --tags --always 2>/dev/null || echo "unknown")
    qb_date=$(git -C "${TARGET_DIR}/qb" log -1 --format=%cs 2>/dev/null || echo "unknown")
    echo "  qb submodule pinned by the template: ${qb_desc} (${qb_date})"
    echo "  This pin comes from ${TEMPLATE}, not from qb. To move it:"
    echo "    git -C '${NAME}/qb' fetch origin && git -C '${NAME}/qb' checkout <ref>"
fi
