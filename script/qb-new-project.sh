#!/usr/bin/env bash

# Scaffold a buildable qb project from the published template.
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
# TEMPLATE is one variable used by the clone, the cd and the cleanup on purpose. Those three
# were three separate literals, and they had already drifted apart: the module script cloned
# `isndev/qbm-sample` (which does not exist) and then cd'd into `qb-sample-module` (which
# does). One name cannot disagree with itself.
set -euo pipefail

TEMPLATE=qb-sample-project
TEMPLATE_URL="https://github.com/isndev/${TEMPLATE}.git"

# Exit if name argument is not given
if [ -z "$*" ]; then
    echo "A project name argument must be provided." >&2
    exit 2
fi

NAME=$1


################################################################################


# Refuse to touch anything that is already there rather than merging into it.
if [ -e "${NAME}" ]; then
    echo "'${NAME}' already exists here -- refusing to overwrite it." >&2
    exit 1
fi
if [ -e "${TEMPLATE}" ]; then
    echo "'${TEMPLATE}' already exists here; move it aside first." >&2
    exit 1
fi

# Clone template repository
git clone "${TEMPLATE_URL}"

# Create bare repository
git --bare init "${NAME}"

# Push template master branch to bare repository
cd "${TEMPLATE}"
git push "../${NAME}" +master:master

# Convert bare repository into a normal repository
cd "../${NAME}"
mkdir .git
mv * .git
git config --local --bool core.bare false
git reset --hard
git submodule update --init --recursive
# Clean Up
rm -rf "../${TEMPLATE}"

echo "Created '${NAME}' from ${TEMPLATE_URL}"
