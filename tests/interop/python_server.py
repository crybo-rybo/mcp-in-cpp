"""A server written with the official Python MCP SDK, which this library's client drives.

    python tests/interop/python_server.py

It serves over stdio, in the era the client opens with: the SDK's stdio server
reads the client's first request, and speaks 2026-07-28 to one that carries the
modern `_meta` envelope and the `initialize` handshake to one that does not.
build/<preset>/tests/mcp_interop_client launches it; see scripts/interop.sh.
"""

import anyio
from pydantic import BaseModel

from mcp.server.mcpserver import Context, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

server = MCPServer(
    name="python-zoo",
    version="2.3.0",
    instructions="A few tools to call from another SDK.",
)

# Set each time a call of wait_for_cancel is cancelled.
cancelled = 0
cancelled_changed = anyio.Event()


class Forecast(BaseModel):
    summary: str
    high: float


@server.tool(description="Get the forecast for a city")
def forecast(city: str, days: int = 1) -> Forecast:
    return Forecast(summary=f"{city} for {days} days", high=21.5)


@server.tool(description="Add two integers")
def add(a: int, b: int) -> int:
    return a + b


@server.tool(description="Refuse every call")
def refuse() -> str:
    raise ToolError("not today")


@server.tool(description="Count to a number, reporting each step")
async def count(steps: int, ctx: Context) -> int:
    for step in range(1, steps + 1):
        await ctx.report_progress(step, steps, f"step {step}")
    return steps


@server.tool(description="Wait until the call is cancelled, reporting once that it runs")
async def wait_for_cancel(ctx: Context) -> str:
    global cancelled, cancelled_changed
    await ctx.report_progress(0, None, "waiting")
    try:
        await anyio.sleep(60)
    except anyio.get_cancelled_exc_class():
        cancelled += 1
        cancelled_changed.set()
        cancelled_changed = anyio.Event()
        raise
    return "not cancelled"


@server.tool(description="How many calls have been cancelled, once there are at least this many")
async def cancellations(at_least: int) -> int:
    with anyio.move_on_after(10):
        while cancelled < at_least:
            await cancelled_changed.wait()
    return cancelled


if __name__ == "__main__":
    server.run("stdio")
