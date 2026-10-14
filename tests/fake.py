"""Scripted test doubles: backend, UI, and voice-stage fakes (no GPU, no mic, no net)."""

from __future__ import annotations

import numpy as np


class ScriptedBackend:
    """Pop replies in order; json_mode uses json_replies. Defaults are always valid."""

    def __init__(self):
        self.replies: list[str] = []
        self.json_replies: list[str] = []
        self.all_messages: list[list[dict]] = []
        self.calls = 0
        self.unloaded = 0

    async def chat(self, messages, *, temperature=0.4, json_mode=False, **_):
        self.all_messages.append([dict(m) for m in messages])
        self.calls += 1
        src = self.json_replies if json_mode else self.replies
        if src:
            return src.pop(0)
        return '{"agent": "writer", "proposal": null}' if json_mode else "FINAL"

    def unload(self):
        self.unloaded += 1


class FakeUI:
    def __init__(self):
        self.events: list[dict] = []
        self.ask_reply = "yes"
        self.proposal_result: dict | None | str = "accept"  # "accept" | "decline" | dict
        self.pull_reply = False
        self.asked = 0

    def event(self, evt: dict) -> None:
        self.events.append(evt)

    def events_of(self, type_: str) -> list[dict]:
        return [e for e in self.events if e.get("type") == type_]

    async def ask(self, agent: str, tool: str, args: dict) -> str:
        self.asked += 1
        return self.ask_reply

    async def confirm_agent(self, proposal: dict, default_model: str) -> dict | None:
        if self.proposal_result == "decline":
            return None
        if self.proposal_result == "accept":
            out = dict(proposal)
            out.setdefault("model", default_model)
            return out
        out = dict(proposal)
        out.update(self.proposal_result)
        return out

    async def confirm_pull(self, model_ref: str) -> bool:
        return self.pull_reply


class FakeWake:
    """Triggers on the Nth frame; records what it was fed."""

    def __init__(self, trigger_at: int = 3):
        self.trigger_at = trigger_at
        self.frames = 0

    def hit(self, frame, now) -> bool:
        self.frames += 1
        return self.frames == self.trigger_at


class FakeVAD:
    """feed() returns True (utterance ended) after `end_after` frames."""

    def __init__(self, end_after: int = 4):
        self.end_after = end_after
        self.frames = 0
        self.resets = 0

    def feed(self, frame) -> bool:
        self.frames += 1
        return self.frames >= self.end_after

    def reset(self):
        self.frames = 0
        self.resets += 1


class FakeSTT:
    def __init__(self, text: str = "run the workspace tests"):
        self.text = text
        self.fed: list[np.ndarray] = []

    def transcribe(self, pcm) -> str:
        self.fed.append(pcm)
        return self.text


def silent_frame(n: int = 1280) -> np.ndarray:
    return np.zeros(n, dtype=np.int16)
