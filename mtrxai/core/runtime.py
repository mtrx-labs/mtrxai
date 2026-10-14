"""Assemble the full runtime (registry, approver, orchestrator) around a UI."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import mtrxai.tools  # noqa: F401 — registers every built-in tool
from ..config import load_env, load_settings
from .approval import Approver
from .orchestrator import Orchestrator
from .registry import Registry
from .trace import Trace


@dataclass
class Runtime:
    settings: dict
    registry: Registry
    approver: Approver
    trace: Trace
    orchestrator: Orchestrator
    ui: object


def assemble(root: Path, ui) -> Runtime:
    load_env(root)
    settings = load_settings(root)
    mtrxai.tools.set_workspace(settings["workspace"])

    trace = Trace(settings["logs"])
    registry = Registry(settings["agents_dir"], settings["providers"], settings.get("gguf", {}))

    def permissions_of(agent_name: str) -> dict:
        la = registry.get(agent_name)
        return la.spec.permissions if la else {}

    approver = Approver(settings, ui, permissions_of)
    orchestrator = Orchestrator(registry, settings, ui, approver, trace)
    return Runtime(
        settings=settings,
        registry=registry,
        approver=approver,
        trace=trace,
        orchestrator=orchestrator,
        ui=ui,
    )
