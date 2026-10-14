"""Shell tool: runs in the workspace, timed, output-truncated, approval-gated."""

from __future__ import annotations

import subprocess

from .base import tool, workspace

MAX_OUTPUT = 8_000
TIMEOUT_S = 120


@tool(
    "run_shell",
    "Run a shell command (bash) with the workspace as cwd; returns exit code, stdout, stderr.",
    {"command": "string"},
    permission="ask",
)
def run_shell(args: dict) -> str:
    try:
        p = subprocess.run(
            args["command"],
            shell=True,
            cwd=workspace(),
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        return f"ERROR: command timed out after {TIMEOUT_S} s: {args['command'][:200]}"
    out = f"exit={p.returncode}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}"
    if len(out) > MAX_OUTPUT:
        out = out[:MAX_OUTPUT] + "\n… truncated"
    return out
