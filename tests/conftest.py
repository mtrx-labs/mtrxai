from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))  # tests/fake.py importable

from fake import FakeUI, ScriptedBackend  # noqa: E402

from mtrxai.config import load_settings  # noqa: E402
from mtrxai.core.approval import Approver  # noqa: E402
from mtrxai.core.orchestrator import Orchestrator  # noqa: E402
from mtrxai.core.registry import Registry  # noqa: E402
from mtrxai.core.trace import Trace  # noqa: E402
from mtrxai.tools import set_workspace  # noqa: E402

ROUTER_YAML = """\
name: router
description: Internal dispatcher.
model: fake:demo
temperature: 0.0
system_prompt: |
  You are the mtrxAI router. Available agents:
  {roster}
  Respond with ONLY {"agent": "<name>"} or {"agent": null, "proposal": {...}}.
tools: []
"""

WRITER_YAML = """\
name: writer
description: Drafts and edits prose — docs, posts, emails, reports.
model: fake:demo
temperature: 0.7
system_prompt: |
  You are Writer. Produce clean prose.
tools: [read_file, write_file, list_dir]
permissions:
  write_file: allow
  read_file: allow
  list_dir: allow
keywords: [write, draft, essay, blog, email, edit, summarize, report]
"""

CODER_YAML = """\
name: coder
description: Writes and runs code.
model: fake:demo
temperature: 0.2
system_prompt: |
  You are Coder.
tools: [read_file, write_file, list_dir, run_shell]
permissions:
  write_file: allow
  read_file: allow
  list_dir: allow
  run_shell: ask
keywords: [code, script, bug, python, test]
"""

SETTINGS = """\
workspace = "./workspace"
logs = "./logs"
agents_dir = "./agents"
max_steps = 2
default_permission = "ask"
router_agent = "router"
default_agent_model = "fake:demo"

[providers.ollama]
base_url = "http://127.0.0.1:11434"
"""


@pytest.fixture
def proj(tmp_path) -> Path:
    root = tmp_path / "proj"
    (root / "agents").mkdir(parents=True)
    (root / "workspace").mkdir()
    (root / "logs").mkdir()
    (root / "settings.toml").write_text(SETTINGS)
    (root / "agents" / "router.yaml").write_text(ROUTER_YAML)
    (root / "agents" / "writer.yaml").write_text(WRITER_YAML)
    (root / "agents" / "coder.yaml").write_text(CODER_YAML)
    settings = load_settings(root)
    set_workspace(settings["workspace"])
    return root


@pytest.fixture
def make_registry(proj):
    """Registry wired to ScriptedBackends; tests script via .backend attributes."""

    def _make():
        def builder(ref, providers, gguf_cfg=None):
            return ScriptedBackend()

        reg = Registry(proj / "agents", {"ollama": {"base_url": "http://127.0.0.1:1"}}, {}, builder)
        return reg

    return _make


@pytest.fixture
def make_orch(proj, make_registry):
    def _make(ui=None):
        ui = ui or FakeUI()
        reg = make_registry()
        settings = load_settings(proj)
        set_workspace(settings["workspace"])
        trace = Trace(settings["logs"])
        approver = Approver(
            settings, ui, lambda name: (reg.get(name).spec.permissions if reg.get(name) else {})
        )
        orch = Orchestrator(reg, settings, ui, approver, trace)
        return orch, ui, reg

    return _make
