#!/usr/bin/env bash
# Runs what CI runs, so a change can be checked before it is pushed: the
# formatting, then for each preset its build and tests and the Python
# interop, and for release the installed package and the conformance suite.
#
#   scripts/ci.sh                 # format, release, asan, and tsan
#   scripts/ci.sh release asan    # format and only these presets
#
# Homebrew's GCC has no thread sanitizer, so on macOS tsan is left out unless
# it is asked for by name.
source "$(dirname "$0")/common.sh"

presets=("$@")
if [[ ${#presets[@]} -eq 0 ]]; then
  presets=(release asan tsan)
  if [[ "$(uname)" == "Darwin" ]]; then
    presets=(release asan)
    echo "macOS: leaving out tsan, which Homebrew's GCC cannot build" >&2
  fi
fi

scripts="$root/scripts"
section "format"
"$scripts/format.sh"
end_section
for preset in "${presets[@]}"; do
  section "build and test ($preset)"
  "$scripts/build.sh" "$preset"
  end_section
  section "interop ($preset)"
  "$scripts/interop.sh" "$preset"
  end_section
  if [[ "$preset" == "release" ]]; then
    section "install"
    "$scripts/install-check.sh" "$preset"
    end_section
    "$scripts/conformance.sh" "$preset"
  fi
done
printf '\nall of CI passed: format %s\n' "${presets[*]}"
