#!/usr/bin/env bash
# Checks the library against the official Python SDK (mcp 2.3.0, installed
# into build/tools), in both protocol eras: the SDK's client drives the example
# server of a built preset over stdio and HTTP, and this library's client
# drives a server written with the SDK over stdio.
#
#   scripts/interop.sh release
source "$(dirname "$0")/common.sh"

preset=${1:?a preset that has been built}
python="$(tool_env python-sdk mcp==2.3.0)/python"
"$python" tests/interop/python_client.py "build/$preset/examples/mcp_weather_example"
"build/$preset/tests/mcp_interop_client" "$python" tests/interop/python_server.py
