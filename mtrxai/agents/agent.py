"""Provider-agnostic agent tool loop.

Small local models are unreliable at native function calling, so agents speak a
JSON-in-text protocol that works with *every* swappable model:

    reply is exactly  {"tool": "<name>", "args": {…}}  → tool call
    reply is anything else                             → final answer
"""

from __future__ import annotations

import asyncio
import json

from ..core.registry import AgentSpec
from ..core.util import extract_json_objects, strip_code_fences
from ..models.base import ModelBackend
from ..tools.base import Toolbox

TOOL_PROTOCOL = """

--- TOOL USE ---
You may call tools. To call one, reply with ONLY this JSON object and nothing else:
{{"tool": "<name>", "args": {{ … }}}}
Tool arguments must match the signatures below. When you have the final answer,
reply with plain text (no JSON object). Available tools:
{tool_docs}
"""


def parse_tool_call(text: str) -> dict | None:
    """A reply is a tool call only if it IS a JSON object with a 'tool' key.

    Deliberately strict (the whole reply must be the object, modulo code fences)
    so prose containing example JSON is never misread as a call.
    """
    s = strip_code_fences(text).strip()
    if not s.startswith("{"):
        return None
    candidates: list[dict] = []
    try:
        whole = json.loads(s)
        if isinstance(whole, dict):
            candidates.append(whole)
    except (json.JSONDecodeError, ValueError):
        pass
    candidates.extend(extract_json_objects(s))
    for obj in candidates:
        if isinstance(obj.get("tool"), str):
            args = obj.get("args")
            return {"tool": obj["tool"].strip(), "args": args if isinstance(args, dict) else {}}
    return None


class Agent:
    def __init__(self, spec: AgentSpec, backend: ModelBackend, approver, ui, max_steps: int = 8, trace=None):
        self.spec = spec
        self.backend = backend
        self.approver = approver
        self.ui = ui
        self.max_steps = max_steps
        self.trace = trace
        self.toolbox = Toolbox(spec.tools) if spec.tools else None

    async def run(self, task: str) -> str:
        system = self.spec.system_prompt
        if self.toolbox is not None and self.toolbox.tools:
            system += TOOL_PROTOCOL.format(tool_docs=self.toolbox.docs())
        msgs = [
            {"role": "system", "content": system},
            {"role": "user", "content": task},
        ]
        self.ui.event({"type": "status", "text": f"{self.spec.name} thinking…"})
        reply = ""
        for step in range(self.max_steps):
            reply = await self.backend.chat(msgs, temperature=self.spec.temperature)
            call = parse_tool_call(reply) if self.toolbox else None
            if call is None:
                return reply

            self.ui.event(
                {"type": "tool_call", "agent": self.spec.name, "tool": call["tool"], "args": call["args"]}
            )
            if self.trace:
                self.trace.log("tool_call", agent=self.spec.name, tool=call["tool"], args=call["args"])

            if call["tool"] not in self.toolbox.names:
                # unknown tool: no approval needed, the error string teaches the model
                result = self.toolbox.execute(call)
            elif await self.approver.check(self.spec.name, call):
                result = await asyncio.to_thread(self.toolbox.execute, call)
            else:
                result = "DENIED by user. Continue without this action."
                self.ui.event(
                    {"type": "tool_denied", "agent": self.spec.name, "tool": call["tool"]}
                )
                if self.trace:
                    self.trace.log("tool_denied", agent=self.spec.name, tool=call["tool"])

            self.ui.event({"type": "tool_result", "result": result[:300]})
            if self.trace:
                self.trace.log("tool_result", agent=self.spec.name, result=result)

            msgs.append({"role": "assistant", "content": reply})
            msgs.append({"role": "user", "content": f"TOOL_RESULT:\n{result}"})

        note = f"(stopped after {self.max_steps} tool steps — here is the last reply)"
        self.ui.event({"type": "status", "text": f"{self.spec.name} hit the step limit"})
        return f"{reply}\n\n{note}"
