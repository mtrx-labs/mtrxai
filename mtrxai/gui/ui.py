"""WebUI: the UI protocol implemented over Server-Sent Events + modal replies.

Same contract as ConsoleUI/TuiUI (see mtrxai.core.ui):
    event()          → fire-and-forget progress broadcast
    ask()            → approval card  → 'yes' | 'always' | 'no'
    confirm_agent()  → proposal card  → edited dict | None
    confirm_pull()   → yes/no question → bool

A card is pushed to every connected browser as a `card` SSE event with a unique
id; the POST /api/card/<id> reply resolves the awaiting future. Threads (e.g.
the voice pipeline, if ever wired here) may call event() — broadcasts are
thread-safe via loop capture.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import threading


class WebUI:
    def __init__(self) -> None:
        self._loop: asyncio.AbstractEventLoop | None = None
        self._subscribers: set[asyncio.Queue] = set()
        self._pending: dict[str, asyncio.Future] = {}
        self._lock = threading.Lock()
        self._ids = itertools.count(1)

    # ------------------------------------------------------------- lifecycle

    def attach(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=512)
        self._subscribers.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subscribers.discard(q)

    def has_subscribers(self) -> bool:
        return bool(self._subscribers)

    def pending_count(self) -> int:
        return len(self._pending)

    def fail_pending(self, reason: str = "browser disconnected") -> int:
        """Resolve every open card so a vanished browser can never hang the agent.

        Denies approvals and declines creation — the safe direction: nothing runs
        unattended just because the window was closed.
        """
        n = 0
        for card_id, fut in list(self._pending.items()):
            if not fut.done():
                fut.set_result({"answer": "no", "action": "cancel", "reason": reason})
                n += 1
        self._pending.clear()
        return n

    # ------------------------------------------------------------- broadcast

    def _push(self, payload: dict) -> None:
        dead: list[asyncio.Queue] = []
        with self._lock:
            subs = list(self._subscribers)
        for q in subs:
            try:
                q.put_nowait(payload)
            except asyncio.QueueFull:
                dead.append(q)
        for q in dead:  # a stalled client must not stall the agent loop
            self._subscribers.discard(q)

    def broadcast(self, payload: dict) -> None:
        """Thread-safe: callable from the loop thread or a worker thread."""
        if self._loop is None:
            return
        try:
            running = asyncio.get_running_loop()
        except RuntimeError:
            running = None
        if running is self._loop:
            self._push(payload)
        else:
            self._loop.call_soon_threadsafe(self._push, payload)

    # ----------------------------------------------------------- UI protocol

    def event(self, evt: dict) -> None:
        self.broadcast({"kind": "event", "evt": evt})

    async def _card(self, payload: dict) -> dict:
        card_id = f"c{next(self._ids)}"
        self.broadcast({"kind": "card", "id": card_id, **payload})
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[card_id] = fut
        try:
            return await fut
        finally:
            self._pending.pop(card_id, None)

    def resolve(self, card_id: str, value: dict) -> bool:
        """Resolve a pending card (called by the HTTP layer). True if delivered."""
        fut = self._pending.get(card_id)
        if fut is None or fut.done():
            return False
        fut.set_result(value)
        return True

    async def ask(self, agent: str, tool: str, args: dict) -> str:
        res = await self._card({
            "card": "approval",
            "title": f"{agent} wants to run {tool}",
            "detail": json.dumps(args, ensure_ascii=False)[:2000],
        })
        return res.get("answer", "no")

    async def confirm_agent(self, proposal: dict, default_model: str) -> dict | None:
        res = await self._card({
            "card": "agent_proposal",
            "proposal": proposal,
            "default_model": default_model,
        })
        if res.get("action") == "cancel":
            return None
        out = {
            "name": str(res.get("name", "")).strip(),
            "description": str(res.get("description", "")).strip(),
            "model": str(res.get("model", default_model)).strip(),
            "tools": res.get("tools") or [],
        }
        return out

    async def confirm_pull(self, model_ref: str) -> bool:
        res = await self._card({
            "card": "confirm",
            "title": f"model '{model_ref.partition(':')[2]}' is not in Ollama.",
            "detail": "Pull it now?",
        })
        return res.get("answer") == "yes"
