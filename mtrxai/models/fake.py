"""Offline deterministic backend — lets the whole system run with zero models.

Reference forms:
    fake:demo   json_mode → routes to 'writer'; otherwise a canned answer
    fake:tools  first reply is a write_file tool call, then a final answer
    fake:error  always raises ModelError (for testing error paths)
"""

from __future__ import annotations

from typing import Any

from .base import ModelError, Msg


class FakeBackend:
    def __init__(self, model: str = "demo"):
        self.model = model
        self.calls = 0

    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, json_mode: bool = False, **_: Any
    ) -> str:
        self.calls += 1
        if self.model == "error":
            raise ModelError("fake:error — simulated model failure")
        if json_mode:
            return '{"agent": "writer", "proposal": null}'
        if self.model == "tools" and self.calls == 1:
            return (
                '{"tool": "write_file", '
                '"args": {"path": "demo.txt", "content": "hello from mtrxAI\\n"}}'
            )
        task = next((m["content"] for m in reversed(messages) if m["role"] == "user"), "")
        return f"[fake:{self.model}] Canned answer. Your task was: {task[:200]}"

    def unload(self) -> None:
        pass
