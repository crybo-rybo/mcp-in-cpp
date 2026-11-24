#!/usr/bin/env bash
# Drives the example server of a built preset through the official Python
# client (mcp 2.3.0, installed into build/tools), over stdio and HTTP, in both
# protocol eras.
#
#   scripts/interop.sh release
source "$(dirname "$0")/common.sh"

preset=${1:?a preset that has been built}
python="$(tool_env python-sdk mcp==2.3.0)/python"
"$python" tests/interop/python_client.py "build/$preset/examples/mcp_weather_example"
