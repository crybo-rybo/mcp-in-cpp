#!/usr/bin/env bash
# Configures, builds, and runs the tests of one preset: dev, release, asan, or
# tsan.
#
#   scripts/build.sh release
source "$(dirname "$0")/common.sh"

cmake --workflow --preset "${1:?a preset: dev, release, asan, or tsan}"
