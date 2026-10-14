"""Agent registry: agents/*.yaml are the source of truth, re-readable at any time.

Re-reading the YAMLs IS the hot-swap primitive. Backends for agents whose model
reference did not change are kept warm across reloads (a loaded .gguf stays in RAM).
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

import yaml

from ..models.base import ModelBackend, ModelError
from ..models.factory import make_backend


@dataclass
class AgentSpec:
    name: str
    description: str
    model: str  # "provider:model"
    system_prompt: str
    tools: list[str] = field(default_factory=list)
    temperature: float = 0.4
    permissions: dict[str, str] = field(default_factory=dict)
    keywords: list[str] = field(default_factory=list)

    _REQUIRED = ("name", "description", "model", "system_prompt")

    @classmethod
    def from_dict(cls, d: dict) -> "AgentSpec":
        if not isinstance(d, dict):
            raise ValueError("agent YAML must be a mapping at top level")
        names = {f.name for f in fields(cls)} - {"_REQUIRED"}
        missing = [k for k in cls._REQUIRED if not d.get(k)]
        if missing:
            raise ValueError(f"missing required field(s): {missing}")
        known = {k: v for k, v in d.items() if k in names}
        return cls(**known)

    def to_yaml(self) -> str:
        return yaml.safe_dump(asdict(self), sort_keys=False, allow_unicode=True, width=100)


@dataclass
class LoadedAgent:
    """A spec plus its (lazily reusable) backend instance."""

    spec: AgentSpec
    backend: ModelBackend


class Registry:
    def __init__(
        self,
        agents_dir: Path,
        providers: dict,
        gguf_cfg: dict | None = None,
        builder=make_backend,
    ):
        self.dir = Path(agents_dir)
        self.providers = providers or {}
        self.gguf_cfg = gguf_cfg or {}
        self._builder = builder
        self._agents: dict[str, LoadedAgent] = {}
        self.warnings: list[str] = []
        self.dir.mkdir(parents=True, exist_ok=True)
        self._mtimes: dict[Path, float] = {}
        self.load()

    # ---------------------------------------------------------------- loading

    def load(self) -> "Registry":
        self.warnings = []
        specs: dict[str, AgentSpec] = {}
        mtimes: dict[Path, float] = {}
        files = sorted(self.dir.glob("*.yaml")) + sorted(self.dir.glob("*.yml"))
        for f in files:
            try:
                spec = AgentSpec.from_dict(yaml.safe_load(f.read_text(encoding="utf-8")))
            except Exception as e:  # noqa: BLE001 — one bad file must not kill the app
                self.warnings.append(f"skipped {f.name}: {e}")
                continue
            if spec.name in specs:
                self.warnings.append(f"duplicate agent name '{spec.name}' ({f.name}) — kept first")
                continue
            specs[spec.name] = spec
            mtimes[f] = f.stat().st_mtime

        # Unload backends only for agents that vanished or changed model; keep warm otherwise.
        for name, la in self._agents.items():
            new = specs.get(name)
            if new is None or new.model != la.spec.model:
                try:
                    la.backend.unload()
                except Exception:
                    pass

        agents: dict[str, LoadedAgent] = {}
        for name, spec in specs.items():
            old = self._agents.get(name)
            if old is not None and old.spec.model == spec.model:
                backend = old.backend  # reuse: no reload cost, gguf stays in RAM
            else:
                try:
                    backend = self._builder(spec.model, self.providers, self.gguf_cfg)
                except ModelError as e:
                    self.warnings.append(f"agent '{name}' model '{spec.model}': {e}")
                    continue
            agents[name] = LoadedAgent(spec=spec, backend=backend)
        self._agents = agents
        self._mtimes = mtimes
        return self

    def reload_if_changed(self) -> bool:
        """Lazy mtime watch — pick up YAML edits without an explicit /reload."""
        current = {}
        files = sorted(self.dir.glob("*.yaml")) + sorted(self.dir.glob("*.yml"))
        for f in files:
            try:
                current[f] = f.stat().st_mtime
            except OSError:
                pass
        if current != self._mtimes:
            self.load()
            return True
        return False

    # ---------------------------------------------------------------- access

    def names(self) -> list[str]:
        return list(self._agents)

    def get(self, name: str) -> LoadedAgent | None:
        return self._agents.get(name)

    def all(self) -> list[LoadedAgent]:
        return list(self._agents.values())

    def worker_names(self, router_name: str = "router") -> list[str]:
        return [n for n in self._agents if n != router_name]

    # ---------------------------------------------------------------- mutation

    def swap(self, name: str, model_ref: str) -> LoadedAgent:
        """Hot-swap an agent's model in memory AND on disk. No restart, ever."""
        la = self._agents.get(name)
        if la is None:
            raise KeyError(f"no agent named '{name}' — /agents lists what exists")
        new_backend = self._builder(model_ref.strip(), self.providers, self.gguf_cfg)  # validates first
        try:
            la.backend.unload()
        except Exception:
            pass
        la.spec.model = model_ref.strip()
        la.backend = new_backend
        (self.dir / f"{name}.yaml").write_text(la.spec.to_yaml(), encoding="utf-8")
        self._mtimes = {}  # force mtime refresh on next lazy check
        return la

    def save_spec(self, spec: AgentSpec) -> LoadedAgent:
        """Write a new agent YAML and reload (used by the agent factory)."""
        path = self.dir / f"{spec.name}.yaml"
        if path.exists():
            raise ValueError(
                f"agent '{spec.name}' already exists — edit {path} and /reload, or pick a new name"
            )
        path.write_text(spec.to_yaml(), encoding="utf-8")
        self.load()
        la = self.get(spec.name)
        if la is None:  # pragma: no cover — only if the builder fails post-write
            raise ModelError(f"agent '{spec.name}' was written but failed to load")
        return la
