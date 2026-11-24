#!/usr/bin/env bash
# Runs the official MCP conformance suite against the fixture server, once for
# each revision the library serves, and fails on any result its baseline does
# not expect.
#
#   tests/conformance/run.sh build/release/tests/mcp_conformance_fixture
#
# It needs Node 22 or newer, and fetches the suite with npx. Set
# MCP_CONFORMANCE_RUNNER to run an installed copy instead.
set -euo pipefail

fixture=$1
here=$(cd "$(dirname "$0")" && pwd)
suite=@modelcontextprotocol/conformance@0.2.0-alpha.12
port=${MCP_CONFORMANCE_PORT:-3917}
out=${MCP_CONFORMANCE_OUT:-conformance-results}
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
  echo "::group::conformance $revision"
  "${runner[@]}" server --url "http://localhost:$port/mcp" \
    --requirements "$revision" \
    --expected-failures "$here/expected-failures.$revision.yml" \
    -o "$out/$revision" || status=1
  echo "::endgroup::"
done
exit "$status"
