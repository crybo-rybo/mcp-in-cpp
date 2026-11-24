#!/usr/bin/env bash
# Runs the official MCP conformance suite against the fixture server of a
# built preset, once for each revision the library serves, and fails on any
# result its baseline (tests/conformance/expected-failures.*.yml) does not
# expect. The results are kept in build/conformance-results.
#
#   scripts/conformance.sh release
#
# It needs Node 22 or newer, and fetches the suite with npx. Set
# MCP_CONFORMANCE_RUNNER to run an installed copy instead.
source "$(dirname "$0")/common.sh"

preset=${1:?a preset that has been built}
fixture="build/$preset/tests/mcp_conformance_fixture"
here="$root/tests/conformance"
node_major=$(node --version 2>/dev/null | sed -E 's/^v([0-9]+).*/\1/' || true)
if [[ -z "$node_major" || "$node_major" -lt 22 ]]; then
  echo "the conformance suite needs Node 22 or newer on PATH" >&2
  exit 1
fi
suite=@modelcontextprotocol/conformance@0.2.0-alpha.12
port=${MCP_CONFORMANCE_PORT:-3917}
out=${MCP_CONFORMANCE_OUT:-$root/build/conformance-results}
read -r -a runner <<<"${MCP_CONFORMANCE_RUNNER:-npx --yes $suite}"

"$fixture" "$port" 2>/dev/null &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
for _ in $(seq 100); do
  if curl -s -o /dev/null --max-time 1 "http://localhost:$port/mcp"; then
    break
  fi
  kill -0 "$server"
  sleep 0.1
done

status=0
for revision in 2026-07-28 2025-11-25; do
  section "conformance $revision"
  "${runner[@]}" server --url "http://localhost:$port/mcp" \
    --requirements "$revision" \
    --expected-failures "$here/expected-failures.$revision.yml" \
    -o "$out/$revision" || status=1
  end_section
done
exit "$status"
