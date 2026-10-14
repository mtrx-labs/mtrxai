import pytest

from mtrxai.core.approval import Approver
from fake import FakeUI

SETTINGS = {"default_permission": "ask"}


def make_approver(ui, permissions, default="ask"):
    return Approver({**SETTINGS, "default_permission": default}, ui, lambda name: permissions)


@pytest.mark.asyncio
async def test_spec_permission_allow_beats_tool_default_ask():
    ui = FakeUI()
    ap = make_approver(ui, {"run_shell": "allow"})
    assert await ap.check("coder", {"tool": "run_shell", "args": {"command": "ls"}}) is True
    assert ui.asked == 0


@pytest.mark.asyncio
async def test_tool_decorator_default_used_without_spec_perm():
    ui = FakeUI()
    ap = make_approver(ui, {})  # web_search decorator default is allow
    assert await ap.check("researcher", {"tool": "web_search", "args": {"query": "x"}}) is True
    assert ui.asked == 0


@pytest.mark.asyncio
async def test_settings_default_applies_to_ask_tools():
    ui = FakeUI()
    ap = make_approver(ui, {}, default="deny")
    assert await ap.check("coder", {"tool": "run_shell", "args": {"command": "ls"}}) is False
    assert ui.asked == 0  # deny never prompts


@pytest.mark.asyncio
async def test_ask_yes_executes():
    ui = FakeUI()
    ui.ask_reply = "yes"
    ap = make_approver(ui, {})
    assert await ap.check("coder", {"tool": "run_shell", "args": {}}) is True
    assert ui.asked == 1


@pytest.mark.asyncio
async def test_ask_no_blocks():
    ui = FakeUI()
    ui.ask_reply = "no"
    ap = make_approver(ui, {})
    assert await ap.check("coder", {"tool": "run_shell", "args": {}}) is False


@pytest.mark.asyncio
async def test_always_grants_rest_of_session():
    ui = FakeUI()
    ui.ask_reply = "always"
    ap = make_approver(ui, {})
    assert await ap.check("coder", {"tool": "run_shell", "args": {}}) is True
    ui.ask_reply = "no"  # would deny if asked again
    assert await ap.check("coder", {"tool": "run_shell", "args": {}}) is True
    assert ui.asked == 1
    ap.forget_session_grants()
    assert await ap.check("coder", {"tool": "run_shell", "args": {}}) is False


@pytest.mark.asyncio
async def test_unknown_tool_never_approved():
    ui = FakeUI()
    ap = make_approver(ui, {"rm_rf": "allow"})
    assert await ap.check("coder", {"tool": "rm_rf", "args": {}}) is False
    assert ui.asked == 0
