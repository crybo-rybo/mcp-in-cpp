#!/usr/bin/env bash
# Installs a built preset into build/prefix, then builds and runs
# tests/install, a project of its own that finds the installed package.
#
#   scripts/install-check.sh release
source "$(dirname "$0")/common.sh"

preset=${1:?a preset that has been built}
rm -rf build/prefix build/consumer
cmake --install "build/$preset" --prefix "$root/build/prefix"
cmake -S tests/install -B build/consumer -G Ninja -DCMAKE_PREFIX_PATH="$root/build/prefix"
cmake --build build/consumer
build/consumer/consumer
