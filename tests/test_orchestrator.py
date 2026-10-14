import json

import pytest

from fake import FakeUI

from mtrxai.models.base import ModelError


@pytest.mark.asyncio
async def test_valid_route_dispatches_named_agent(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = ['{"agent": "writer", "proposal": null}']
    answer = await orch.dispatch("hello")
    assert answer == "FINAL"
    routed = ui.events_of("routed")
    assert routed and routed[0]["agent"] == "writer"
    assert any(e["type"] == "answer" for e in ui.events)
    assert any(e.get("chosen") == "writer" for e in orch.trace.last if e["type"] == "route")


@pytest.mark.asyncio
async def test_router_prompt_contains_roster(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = ['{"agent": "coder"}']
    await orch.dispatch("fix this python bug")
    system = reg.get("router").backend.all_messages[0][0]["content"]
    assert "writer" in system and "coder" in system
    assert "router" not in system.split("Available agents:")[1].split("\n")[1]  # not self-listed


@pytest.mark.asyncio
async def test_invalid_then_valid_json_uses_repair_retry(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = [
        "I think the writer should… definitely!",
        '{"agent": "writer"}',
    ]
    answer = await orch.dispatch("write a poem")
    assert answer == "FINAL"
    assert reg.get("router").backend.calls == 2


@pytest.mark.asyncio
async def test_invalid_twice_falls_back_to_keywords(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = ["garbage", "more garbage"]
    answer = await orch.dispatch("please write me a short essay")
    assert answer == "FINAL"
    assert ui.events_of("routing_fallback")
    assert any(e.get("chosen") == "writer" for e in orch.trace.last if e["type"] == "route")


@pytest.mark.asyncio
async def test_unknown_agent_name_repaired_then_fallback(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = ['{"agent": "does-not-exist"}', '{"agent": "ghost"}']
    answer = await orch.dispatch("write something")
    assert answer == "FINAL"  # heuristic saved the run


@pytest.mark.asyncio
async def test_router_model_down_falls_back_silently(proj, make_orch):
    orch, ui, reg = make_orch()

    class Down:
        async def chat(self, *a, **k):
            raise ModelError("Ollama is not reachable")

        def unload(self):
            pass

    reg.get("router").backend = Down()
    answer = await orch.dispatch("write me a haiku")
    assert answer == "FINAL"
    assert ui.events_of("routing_fallback")


@pytest.mark.asyncio
async def test_proposal_creates_agent_after_approval(proj, make_orch):
    orch, ui, reg = make_orch()
    ui.proposal_result = {
        "name": "data-analyst",
        "description": "Analyzes CSV files",
        "tools": ["read_file", "list_dir"],
        "model": "fake:demo",
    }
    reg.get("router").backend.json_replies = [
        json.dumps({
            "agent": None,
            "proposal": {"name": "data-analyst", "description": "Analyze CSV files",
                         "tools": ["read_file"]},
        })
    ]
    answer = await orch.dispatch("analyze this CSV")
    assert answer == "FINAL"
    assert (proj / "agents" / "data-analyst.yaml").is_file()
    assert ui.events_of("agent_created")
    assert reg.get("data-analyst") is not None

    # the new agent now appears in the router's roster on the next dispatch
    reg.get("router").backend.json_replies = ['{"agent": "data-analyst"}']
    await orch.dispatch("analyze another CSV")
    system = reg.get("router").backend.all_messages[-1][0]["content"]
    assert "data-analyst" in system
    assert ui.events_of("routed")[-1]["agent"] == "data-analyst"


@pytest.mark.asyncio
async def test_declined_proposal_reports_back(proj, make_orch):
    orch, ui, reg = make_orch()
    ui.proposal_result = "decline"
    reg.get("router").backend.json_replies = [
        '{"agent": null, "proposal": {"name": "chef", "description": "cooks", "tools": []}}'
    ]
    answer = await orch.dispatch("make me dinner")
    assert "declined" in answer
    assert not (proj / "agents" / "chef.yaml").exists()
    assert ui.events_of("agent_declined")


@pytest.mark.asyncio
async def test_agent_model_error_is_user_friendly(proj, make_orch):
    orch, ui, reg = make_orch()
    reg.get("router").backend.json_replies = ['{"agent": "writer"}']

    class Down:
        async def chat(self, *a, **k):
            raise ModelError("model 'x' is not in Ollama — run: ollama pull x")

        def unload(self):
            pass

    reg.get("writer").backend = Down()
    answer = await orch.dispatch("write a poem")
    assert "⚠" in answer and "ollama" in answer
    assert ui.events_of("error")


@pytest.mark.asyncio
async def test_trace_written_to_jsonl(proj, make_orch):
    orch, ui, reg = make_orch()
    await orch.dispatch("hello")
    lines = (proj / "logs" / "trace.jsonl").read_text().strip().splitlines()
    types = [json.loads(ln)["type"] for ln in lines]
    assert "task" in types and "route" in types and "answer" in types
