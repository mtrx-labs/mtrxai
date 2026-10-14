"""Tool registry: @tool decorator, workspace root, and the per-agent Toolbox."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ToolFn = Callable[[dict], str]


@dataclass
class Tool:
    name: str
    description: str
    args: dict[str, str]  # arg name → type description, shown to the model
    permission: str  # default policy: allow | ask | deny (agent spec can override)
    fn: ToolFn


TOOLBOX: dict[str, Tool] = {}

_workspace: Path = Path.cwd()


def set_workspace(path: Path) -> None:
    global _workspace
    _workspace = Path(path).resolve()


def workspace() -> Path:
    return _workspace


def tool(name: str, description: str, args: dict[str, str], permission: str = "ask") -> Callable[[ToolFn], ToolFn]:
    def deco(fn: ToolFn) -> ToolFn:
        TOOLBOX[name] = Tool(name, description, args, permission, fn)
        return fn

    return deco


class Toolbox:
    """The subset of tools one agent may use, plus validation and execution.

    execute() never raises: errors come back as 'ERROR: …' strings so the model
    can react and the tool loop keeps running.
    """

    def __init__(self, names: list[str]):
        unknown = [n for n in names if n not in TOOLBOX]
        if unknown:
            raise ValueError(
                f"unknown tool(s) {unknown} — available: {', '.join(sorted(TOOLBOX))}"
            )
        self.tools = [TOOLBOX[n] for n in names]

    @property
    def names(self) -> list[str]:
        return [t.name for t in self.tools]

    def docs(self) -> str:
        lines = []
        for t in self.tools:
            args = ", ".join(f"{k}: {v}" for k, v in t.args.items())
            lines.append(f"- {t.name}({args}) — {t.description}")
        return "\n".join(lines)

    def execute(self, call: dict) -> str:
        name = call.get("tool")
        args = call.get("args") or {}
        t = TOOLBOX.get(name)
        if t is None:
            return f"ERROR: unknown tool '{name}'. Available: {', '.join(sorted(TOOLBOX))}"
        if not isinstance(args, dict):
            return "ERROR: 'args' must be a JSON object"
        missing = [k for k in t.args if "optional" not in t.args[k] and k not in args]
        if missing:
            return f"ERROR: {name} is missing arg(s): {missing}"
        try:
            out = t.fn(args)
        except Exception as e:  # noqa: BLE001 — tool errors are model-visible strings
            return f"ERROR: {type(e).__name__}: {e}"
        return out if isinstance(out, str) else json.dumps(out)
