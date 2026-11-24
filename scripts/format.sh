#!/usr/bin/env bash
# Checks that every C++ source follows .clang-format, as clang-format 22.1.4
# applies it: the version CI uses, installed into build/tools. With --fix it
# formats them instead.
#
#   scripts/format.sh [--fix]
source "$(dirname "$0")/common.sh"

clang_format="$(tool_env clang-format clang-format==22.1.4)/clang-format"
if [[ "${1:-}" == "--fix" ]]; then
  git ls-files -z '*.cpp' '*.hpp' | xargs -0 "$clang_format" -i
  echo "formatted"
else
  git ls-files -z '*.cpp' '*.hpp' | xargs -0 "$clang_format" --dry-run --Werror
  echo "the formatting is clean"
fi
