"""The mtrxAI brain: route each task, dispatch, and create new agents with approval."""

from __future__ import annotations

import asyncio
import json
import subprocess

from ..models.base import ModelError
from ..models.ollama import OllamaBackend
from .registry import LoadedAgent, Registry
from .util import first_json_dict
from .trace import Trace
from ..agents.agent import Agent
from ..agents.factory import create_agent_spec


class Orchestrator:
    def __init__(self, registry: Registry, settings: dict, ui, approver, trace: Trace):
        self.registry = registry
        self.settings = settings
        self.ui = ui
        self.approver = approver
        self.trace = trace
        self.router_name = settings.get("router_agent", "router")
        self.max_steps = int(settings.get("max_steps", 8))
        self.default_model = settings.get("default_agent_model", "ollama:llama3.1:8b")

    # ------------------------------------------------------------------ dispatch

    async def dispatch(self, task: str, source: str = "text") -> str:
        self.trace.log("task", source=source, task=task)
        self.registry.reload_if_changed()

        decision = await self._route(task)
        la = self.registry.get(decision["agent"]) if decision.get("agent") else None
        if la is None:
            la = await self.create_flow(decision.get("proposal") or {})
            if la is None:
                msg = "No existing agent fits this task and creating one was declined."
                self.trace.log("answer", agent="(declined)", text=msg)
                self.ui.event({"type": "answer", "agent": "mtrxAI", "text": msg})
                return msg

        self.ui.event({"type": "routed", "agent": la.spec.name, "model": la.spec.model})
        self.trace.log("dispatch", agent=la.spec.name, model=la.spec.model)
        answer = await self._run_agent(la, task)
        self.trace.log("answer", agent=la.spec.name, text=answer[:4000])
        self.ui.event({"type": "answer", "agent": la.spec.name, "text": answer})
        return answer

    async def _run_agent(self, la: LoadedAgent, task: str) -> str:
        try:
            agent = Agent(
                spec=la.spec,
                backend=la.backend,
                approver=self.approver,
                ui=self.ui,
                max_steps=self.max_steps,
                trace=self.trace,
            )
        except ValueError as e:  # unknown tool names in the YAML
            msg = f"agent '{la.spec.name}' has a bad tools list: {e}"
            self.ui.event({"type": "error", "text": msg})
            return msg
        try:
            return await agent.run(task)
        except ModelError as e:
            msg = f"⚠ {la.spec.name} failed: {e}"
            self.ui.event({"type": "error", "text": msg})
            self.trace.log("model_error", agent=la.spec.name, error=str(e))
            return msg

    # ------------------------------------------------------------------ routing

    async def _route(self, task: str) -> dict:
        router = self.registry.get(self.router_name)
        workers = self.registry.worker_names(self.router_name)
        if router is None:
            if not workers:
                return {"agent": None, "proposal": _default_proposal()}
            return self._heuristic(task)

        roster = "\n".join(
            f"- {self.registry.get(n).spec.name}: {self.registry.get(n).spec.description}"
            for n in workers
        ) or "- (none)"
        system = router.spec.system_prompt.replace("{roster}", roster)
        feedback = ""
        for attempt in range(2):
            messages = [
                {"role": "system", "content": system + feedback},
                {"role": "user", "content": task},
            ]
            try:
                raw = await router.backend.chat(
                    messages, temperature=router.spec.temperature, json_mode=True
                )
            except ModelError as e:
                self.ui.event({"type": "routing_fallback", "reason": str(e)})
                self.trace.log("route_fallback", reason=str(e))
                return self._heuristic(task)
            d = _parse_decision(raw)
            if d is not None:
                if d["agent"] is None or d["agent"] in workers:
                    self.trace.log(
                        "route", chosen=d["agent"], proposal=d["proposal"], raw=raw[:300]
                    )
                    if d["agent"] is None:
                        self.ui.event({"type": "proposal"})
                    return d
                feedback = (
                    f"\nAgent '{d['agent']}' does not exist. Choose from the list, "
                    f"or propose a new agent."
                )
            else:
                feedback = (
                    "\nYour previous reply was not a valid decision. "
                    "Respond with ONLY the JSON object."
                )
        self.ui.event({"type": "routing_fallback", "reason": "invalid JSON after retry"})
        self.trace.log("route_fallback", reason="invalid JSON after retry")
        return self._heuristic(task)

    def _heuristic(self, task: str) -> dict:
        t = task.lower()
        best, score = None, 0
        for la in self.registry.all():
            if la.spec.name == self.router_name:
                continue
            s = sum(kw in t for kw in la.spec.keywords)
            if s > score:
                best, score = la.spec.name, s
        if best is None:
            workers = self.registry.worker_names(self.router_name)
            best = workers[0] if workers else None
        if best is None:
            return {"agent": None, "proposal": _default_proposal()}
        self.trace.log("route", chosen=best, raw="heuristic")
        return {"agent": best, "proposal": None}

    # ------------------------------------------------------- agent creation flow

    async def create_flow(self, proposal: dict) -> LoadedAgent | None:
        """Approval card → (optional edits) → YAML on disk → registry reload → agent."""
        draft = dict(proposal)
        if not draft.get("name"):
            draft = _default_proposal()
        edited = await self.ui.confirm_agent(draft, self.default_model)
        if edited is None:
            self.ui.event({"type": "agent_declined"})
            self.trace.log("agent_declined", name=draft.get("name"))
            return None
        try:
            spec = create_agent_spec(edited, self.default_model)
        except ValueError as e:
            self.ui.event({"type": "error", "text": f"invalid proposal: {e}"})
            return None

        if spec.model.startswith("ollama:"):
            await self._ensure_ollama_model(spec.model)

        try:
            la = self.registry.save_spec(spec)
        except (ValueError, ModelError) as e:
            self.ui.event({"type": "error", "text": str(e)})
            return None
        self.ui.event({"type": "agent_created", "name": spec.name, "model": spec.model})
        self.trace.log("agent_created", name=spec.name, model=spec.model, tools=spec.tools)
        return la

    async def _ensure_ollama_model(self, model_ref: str) -> None:
        model = model_ref.partition(":")[2]
        base = (self.settings.get("providers", {}).get("ollama") or {}).get(
            "base_url", "http://127.0.0.1:11434"
        )
        if not await OllamaBackend.list_models(base):
            return  # server down — surface the pull hint later via the backend error
        names = await OllamaBackend.list_models(base)
        if model in names or f"{model}:latest" in names:
            return
        if not await self.ui.confirm_pull(model_ref):
            return
        self.ui.event({"type": "pull", "line": f"ollama pull {model}"})
        try:
            proc = await asyncio.to_thread(
                subprocess.run,
                ["ollama", "pull", model],
                capture_output=True,
                text=True,
                timeout=1800,
            )
            for line in (proc.stdout or "").splitlines()[-5:]:
                self.ui.event({"type": "pull", "line": line})
            if proc.returncode != 0:
                self.ui.event(
                    {"type": "error", "text": f"ollama pull failed: {proc.stderr[-300:]}"}
                )
        except (OSError, subprocess.SubprocessError) as e:
            self.ui.event({"type": "error", "text": f"ollama pull failed: {e}"})


def _parse_decision(raw: str) -> dict | None:
    obj = first_json_dict(raw)
    if obj is None or "agent" not in obj:
        return None
    agent = obj["agent"]
    if agent is None:
        proposal = obj.get("proposal")
        return {"agent": None, "proposal": proposal if isinstance(proposal, dict) else {}}
    if isinstance(agent, str) and agent.strip():
        return {"agent": agent.strip(), "proposal": obj.get("proposal")}
    return None


def _default_proposal() -> dict:
    return {
        "name": "assistant",
        "description": "General-purpose assistant for tasks that fit no specialist.",
        "tools": [],
    }
