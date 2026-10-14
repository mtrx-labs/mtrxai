import json

import pytest

from fake import FakeUI

from mtrxai.agents.agent import Agent, parse_tool_call
from mtrxai.core.approval import Approver
from mtrxai.core.trace import Trace
from mtrxai.models.base import ModelError


# --------------------------------------------------------------- parse_tool_call

def test_parse_plain_json():
    assert parse_tool_call('{"tool": "read_file", "args": {"path": "a"}}') == {
        "tool": "read_file", "args": {"path": "a"}
    }


def test_parse_fenced_json():
    fenced = '```json\n{"tool": "list_dir", "args": {}}\n```'
    assert parse_tool_call(fenced) == {"tool": "list_dir", "args": {}}


def test_prose_is_never_a_tool_call():
    assert parse_tool_call("Here is your JSON example: {\"tool\": \"x\"} — enjoy!") is None


def test_json_without_tool_key_is_final():
    assert parse_tool_call('{"answer": "42"}') is None


def test_args_defaults_to_empty_and_bad_args_coerced():
    assert parse_tool_call('{"tool": "list_dir", "args": "junk"}') == {
        "tool": "list_dir", "args": {}
    }


# --------------------------------------------------------------- the tool loop


def build_agent(make_orch, name="writer", max_steps=3):
    orch, ui, reg = make_orch()
    la = reg.get(name)
    agent = Agent(la.spec, la.backend, orch.approver, ui, max_steps=max_steps, trace=orch.trace)
    return agent, la, ui, reg


@pytest.mark.asyncio
async def test_tool_call_executes_and_feeds_result(proj, make_orch):
    agent, la, ui, reg = build_agent(make_orch)
    la.backend.replies = [
        '{"tool": "write_file", "args": {"path": "a.txt", "content": "hi"}}',
        "done — file written",
    ]
    answer = await agent.run("write hi to a.txt")
    assert answer == "done — file written"
    assert (proj / "workspace" / "a.txt").read_text() == "hi"
    tool_msgs = la.backend.all_messages[-1]
    assert any("TOOL_RESULT" in m["content"] and "wrote" in m["content"] for m in tool_msgs)
    assert ui.events_of("tool_call") and ui.events_of("tool_result")


@pytest.mark.asyncio
async def test_denied_tool_injects_denial(proj, make_orch):
    agent, la, ui, reg = build_agent(make_orch, name="coder")
    ui.ask_reply = "no"
    la.backend.replies = [
        '{"tool": "run_shell", "args": {"command": "rm -rf /"}}',
        "okay, I continued without it",
    ]
    answer = await agent.run("delete everything")
    assert "continued without" in answer
    assert ui.events_of("tool_denied")
    assert any("DENIED by user" in m["content"] for m in la.backend.all_messages[-1])


@pytest.mark.asyncio
async def test_unknown_tool_gets_error_not_approval(proj, make_orch):
    agent, la, ui, reg = build_agent(make_orch)
    la.backend.replies = [
        '{"tool": "not_a_tool", "args": {}}',
        "recovered",
    ]
    answer = await agent.run("do a thing")
    assert answer == "recovered"
    assert ui.asked == 0  # unknown tools bypass the gate; error string teaches the model
    assert any("unknown tool" in e["result"] for e in ui.events_of("tool_result"))


@pytest.mark.asyncio
async def test_step_limit_stops_loop(proj, make_orch):
    agent, la, ui, reg = build_agent(make_orch, max_steps=2)
    la.backend.replies = ['{"tool": "list_dir", "args": {}}'] * 10
    answer = await agent.run("loop forever")
    assert "stopped after 2 tool steps" in answer
    assert la.backend.calls == 2


@pytest.mark.asyncio
async def test_model_error_propagates_to_caller(proj, make_orch):
    agent, la, ui, reg = build_agent(make_orch)

    class Exploding:
        async def chat(self, *a, **k):
            raise ModelError("boom")

        def unload(self):
            pass

    agent.backend = Exploding()
    with pytest.raises(ModelError, match="boom"):
        await agent.run("anything")


@pytest.mark.asyncio
async def test_no_tools_agent_returns_plain_reply(proj, make_orch):
    orch, ui, reg = make_orch()
    la = reg.get("router")
    la.backend.replies = ['{"agent": "writer"}']  # would parse as JSON, but no tools
    agent = Agent(la.spec, la.backend, orch.approver, ui, max_steps=3, trace=orch.trace)
    assert await agent.run("route me") == '{"agent": "writer"}'
