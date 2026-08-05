#!/usr/bin/env bash

# Scaffold a qbm module from the published template.
#
# The clone URL here was WRONG: it named `isndev/qbm-sample`, which does not exist, while the
# `cd` two steps later named `qb-sample-module`, which does. Verified with `gh repo view` on
# both. The mismatch survived because the two were separate string literals; TEMPLATE below is
# now the single source for the clone, the cd and the cleanup, so they cannot drift again.
#
# The 404 was also not a clean failure. Without `set -e` the clone failed, the `cd` failed, and
# `mkdir .git; mv * .git` then ran against the user's INVOCATION directory and swallowed its
# contents -- exiting 0. Reproduced end to end before this was written. Hence the guards below;
# see qb-new-project.sh, which had the identical structure and the identical exposure.
set -euo pipefail

TEMPLATE=qb-sample-module
TEMPLATE_URL="https://github.com/isndev/${TEMPLATE}.git"

# Exit if name argument is not given
if [ -z "$*" ]; then
    echo "A module name argument must be provided." >&2
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
