"""Drives a server built with this library through the official Python MCP client.

    python tests/interop/python_client.py build/dev/examples/mcp_weather_example

It connects three times: as the client connects by default (a `server/discover`
probe, then the 2026-07-28 revision), with the `initialize` handshake of the
earlier revisions, and pinned to 2026-07-28 without a probe. Each time it lists
the tools and calls them, and checks what the client made of every answer. It
needs `pip install mcp` (2.3 or newer), and exits non-zero on the first
difference.
"""

import asyncio
import sys
import time

import anyio

from mcp import Client, StdioServerParameters
from mcp.shared.exceptions import MCPError


def check(label: str, actual: object, expected: object) -> None:
    if actual != expected:
        raise SystemExit(f"FAIL {label}: got {actual!r}, expected {expected!r}")
    print(f"ok   {label}")


async def exercise(
    command: str, mode: str, version: str, modern: bool, pinned: bool = False
) -> None:
    tag = f"[{mode}]"
    progress: list[tuple[float, float | None, str | None]] = []

    async def on_progress(
        value: float, total: float | None, message: str | None
    ) -> None:
        progress.append((value, total, message))

    async with Client(StdioServerParameters(command=command), mode=mode) as client:
        check(f"{tag} protocol version", client.protocol_version, version)
        if not pinned:
            # A pinned client sends no discovery request, so it knows the
            # server only by the answers to its calls.
            info = client.server_info
            check(f"{tag} server name", info and info.name, "weather")
            check(f"{tag} server version", info and info.version, "1.0.0")
            check(
                f"{tag} instructions",
                client.instructions,
                "Forecasts for a few cities. Call cities first.",
            )
            check(
                f"{tag} tools capability",
                client.server_capabilities.tools is not None,
                True,
            )

        tools = {tool.name: tool for tool in (await client.list_tools()).tools}
        check(
            f"{tag} tool names",
            list(tools),
            ["forecast", "cities", "download_archive", "watch"],
        )
        check(
            f"{tag} forecast description",
            tools["forecast"].description,
            "Get the forecast for a city",
        )
        check(
            f"{tag} forecast input schema",
            tools["forecast"].input_schema,
            {
                "additionalProperties": False,
                "properties": {
                    "city": {"description": "City name", "type": "string"},
                    "days": {"maximum": 7, "minimum": 1, "type": "integer"},
                },
                "required": ["city"],
                "type": "object",
            },
        )
        check(
            f"{tag} forecast output schema is an object's",
            (tools["forecast"].output_schema or {}).get("type"),
            "object",
        )
        # Only the modern revision allows an output schema that is not an object's.
        check(
            f"{tag} cities output schema",
            tools["cities"].output_schema,
            {"items": {"type": "string"}, "type": "array"} if modern else None,
        )

        result = await client.call_tool("forecast", {"city": "Oslo", "days": 2})
        check(f"{tag} forecast is not an error", result.is_error, False)
        check(
            f"{tag} forecast structured content",
            result.structured_content,
            {"sky": "clear", "temperature": 19.5},
        )
        check(
            f"{tag} forecast text",
            result.content[0].text,
            '{"sky":"clear","temperature":19.5}',
        )

        result = await client.call_tool("forecast", {"city": "Oslo"})
        check(
            f"{tag} a default argument applies",
            result.structured_content,
            {"sky": "clear", "temperature": 20.5},
        )

        result = await client.call_tool("forecast", {"city": "Oslo", "days": 9})
        check(f"{tag} bad arguments are a tool error", result.is_error, True)
        check(f"{tag} bad arguments text", result.content[0].text, "$.days must be at most 7")

        result = await client.call_tool("forecast", {"city": ""})
        check(f"{tag} a refusal is a tool error", result.is_error, True)
        check(f"{tag} refusal text", result.content[0].text, "city must not be empty")

        result = await client.call_tool("cities", {})
        check(f"{tag} cities text", result.content[0].text, '["Lima","Oslo"]')
        check(
            f"{tag} cities structured content",
            result.structured_content,
            ["Lima", "Oslo"] if modern else None,
        )

        result = await client.call_tool(
            "download_archive", {"city": "Lima"}, progress_callback=on_progress
        )
        check(f"{tag} a string is sent as text", result.content[0].text, "archive of Lima is ready")
        check(
            f"{tag} progress reports",
            progress,
            [(1.0, 3.0, "downloading"), (2.0, 3.0, "downloading"), (3.0, 3.0, "downloading")],
        )

        # Two calls of a second each run side by side.
        started = time.monotonic()
        async with anyio.create_task_group() as group:
            for city in ("Lima", "Oslo"):
                group.start_soon(client.call_tool, "watch", {"city": city, "seconds": 1})
        check(f"{tag} calls run side by side", time.monotonic() - started < 1.8, True)

        # A call the client gives up on is cancelled, and the server goes on.
        with anyio.move_on_after(0.3):
            await client.call_tool("watch", {"city": "Lima", "seconds": 60})
            raise SystemExit(f"FAIL {tag} a call of a minute returned at once")
        result = await client.call_tool("watch", {"city": "Oslo", "seconds": 0})
        check(f"{tag} served after a cancellation", result.content[0].text, "no storm warnings for Oslo")

        try:
            await client.call_tool("fly", {})
        except MCPError as error:
            check(f"{tag} unknown tool code", error.error.code, -32602)
            check(f"{tag} unknown tool message", error.error.message, "Unknown tool: fly")
        else:
            raise SystemExit(f"FAIL {tag} an unknown tool did not raise")


async def main() -> None:
    command = sys.argv[1]
    await exercise(command, "auto", "2026-07-28", modern=True)
    await exercise(command, "legacy", "2025-11-25", modern=False)
    await exercise(command, "2026-07-28", "2026-07-28", modern=True, pinned=True)
    print("all interop checks passed")


asyncio.run(main())
