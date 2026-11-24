# Sourced by the other scripts: where the repository is, which compiler to
# build with, and a Python environment for a tool.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

# The library needs GCC 16.2. Where `g++` is another compiler, as on macOS,
# use `g++-16` when there is one. Set CXX to choose another.
if [[ -z "${CXX:-}" ]] && command -v g++-16 >/dev/null &&
  ! g++ --version 2>/dev/null | grep -q ' 16\.'; then
  export CXX=g++-16
fi

# A Python environment of one tool, kept under build/tools so that it is made
# once. $1 names it, and the rest are what pip installs into it. Prints the
# environment's bin directory. Set PYTHON to make it with another Python.
tool_env() {
  local name=$1
  shift
  local dir="$root/build/tools/$name"
  local stamp="$dir/.installed"
  if [[ ! -f "$stamp" || "$(cat "$stamp")" != "$*" ]]; then
    rm -rf "$dir"
    "${PYTHON:-python3}" -m venv "$dir" >&2
    "$dir/bin/pip" install --quiet "$@" >&2
    echo "$*" >"$stamp"
  fi
  echo "$dir/bin"
}

section() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::group::$*"
  else
    printf '\n\033[1m== %s\033[0m\n' "$*"
  fi
}

end_section() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::endgroup::"
  fi
}
