#!/usr/bin/env bash

# Exit if name argument is not given
if [ -z "$*" ]; then
    echo "A module name argument must be provided."
    exit 0
fi

NAME=$1


################################################################################


# Clone template repository
git clone https://github.com/isndev/qb-sample-module.git

# Create bare repository
git --bare init ${NAME}

# Push template master branch to bare repository
cd qb-sample-module
git push ../${NAME} +master:master

# Convert bare repository into a normal repository
cd ../${NAME}
mkdir .git
mv * .git
git config --local --bool core.bare false
git reset --hard
git submodule update --init --recursive
# Clean Up
rm -rf ../qb-sample-module
