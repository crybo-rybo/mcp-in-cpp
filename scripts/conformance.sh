#!/usr/bin/env bash
# Runs the official MCP conformance suite against a built preset, once for
# each revision the library speaks: in server mode against the fixture server,
# and in client mode against the conformance client. It fails on any result a
# baseline (tests/conformance/expected-failures.*.yml) does not expect. The
# results are kept in build/conformance-results.
#
#   scripts/conformance.sh release
#
# It needs Node 22 or newer, and fetches the suite with npx. Set
# MCP_CONFORMANCE_RUNNER to run an installed copy instead.
source "$(dirname "$0")/common.sh"

preset=${1:?a preset that has been built}
fixture="build/$preset/tests/mcp_conformance_fixture"
client="$root/build/$preset/tests/mcp_conformance_client"
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
# The client scenarios a revision runs without scoring them, which the client
# passes all the same, and which therefore must not start to fail.
unscored_client=(json-schema-2020-12-preservation)

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
  baseline="$here/expected-failures.$revision.yml"
  section "conformance $revision: server"
  "${runner[@]}" server --url "http://localhost:$port/mcp" \
    --requirements "$revision" --expected-failures "$baseline" \
    -o "$out/$revision" || status=1
  end_section
  section "conformance $revision: client"
  "${runner[@]}" client --command "$client" \
    --requirements "$revision" --expected-failures "$baseline" \
    -o "$out/client-$revision" || status=1
  for scenario in "${unscored_client[@]}"; do
    "${runner[@]}" client --command "$client" --scenario "$scenario" \
      --spec-version "$revision" --expected-failures "$baseline" \
      -o "$out/client-$revision" || status=1
  done
  end_section
done
exit "$status"
