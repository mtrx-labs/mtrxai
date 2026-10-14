from pathlib import Path

import pytest

from mtrxai.core.registry import Registry

ROUTER = """\
name: router
description: dispatcher
model: fake:demo
system_prompt: route {roster}
tools: []
"""

WRITER = """\
name: writer
description: prose
model: fake:demo
system_prompt: write
tools: []
keywords: [write]
"""


def write(dir: Path, name: str, text: str) -> None:
    (dir / f"{name}.yaml").write_text(text)


@pytest.fixture
def agents_dir(tmp_path):
    d = tmp_path / "agents"
    d.mkdir()
    write(d, "router", ROUTER)
    write(d, "writer", WRITER)
    return d


def make(agents_dir):
    def builder(ref, providers, gguf_cfg=None):
        from fake import ScriptedBackend

        return ScriptedBackend()

    return Registry(agents_dir, {}, {}, builder)


def test_load_reads_all_yamls(agents_dir):
    reg = make(agents_dir)
    assert reg.names() == ["router", "writer"]


def test_bad_yaml_is_skipped_with_warning(agents_dir):
    write(agents_dir, "broken", "{ not: : yaml")
    reg = make(agents_dir)
    assert "broken" not in reg.names()
    assert any("broken" in w for w in reg.warnings)


def test_missing_required_field_warns(agents_dir):
    write(agents_dir, "minimal", "name: minimal\ndescription: no model\n")
    reg = make(agents_dir)
    assert "minimal" not in reg.names()
    assert any("minimal" in w for w in reg.warnings)


def test_swap_replaces_backend_and_persists_yaml(agents_dir):
    reg = make(agents_dir)
    old = reg.get("writer").backend
    la = reg.swap("writer", "fake:other")
    assert la.spec.model == "fake:other"
    assert reg.get("writer").backend is not old
    assert old.unloaded == 1
    text = (agents_dir / "writer.yaml").read_text()
    assert "fake:other" in text
    assert "name: writer" in text  # spec survives the round-trip


def test_swap_unknown_agent_raises(agents_dir):
    reg = make(agents_dir)
    with pytest.raises(KeyError):
        reg.swap("ghost", "fake:demo")


def test_reload_if_changed_picks_up_edits(agents_dir):
    reg = make(agents_dir)
    assert reg.reload_if_changed() is False  # nothing touched
    write(agents_dir, "writer", ROUTER.replace("name: router", "name: writer").replace(
        "dispatcher", "prose v2").replace("route", "write"))
    assert reg.reload_if_changed() is True
    assert reg.get("writer").spec.description == "prose v2"


def test_reload_keeps_warm_backend_when_model_unchanged(agents_dir):
    reg = make(agents_dir)
    backend = reg.get("writer").backend
    write(agents_dir, "writer", WRITER.replace("description: prose",
                                               "description: prose edited"))
    reg.load()
    assert reg.get("writer").backend is backend  # not rebuilt, not unloaded


def test_reload_unloads_backend_when_model_changes(agents_dir):
    reg = make(agents_dir)
    backend = reg.get("writer").backend
    write(agents_dir, "writer", WRITER.replace("model: fake:demo", "model: fake:two"))
    reg.load()
    assert backend.unloaded == 1
    assert reg.get("writer").spec.model == "fake:two"


def test_save_spec_rejects_duplicates(agents_dir):
    from mtrxai.core.registry import AgentSpec

    reg = make(agents_dir)
    dup = AgentSpec(name="writer", description="d", model="fake:demo", system_prompt="s")
    with pytest.raises(ValueError, match="already exists"):
        reg.save_spec(dup)

    fresh = AgentSpec(name="data-analyst", description="d", model="fake:demo",
                      system_prompt="s", tools=["list_dir"])
    la = reg.save_spec(fresh)
    assert la.spec.name == "data-analyst"
    assert (agents_dir / "data-analyst.yaml").is_file()
