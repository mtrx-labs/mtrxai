"""Web GUI tests: palette contract, WebUI protocol, and live-server integration.

Everything runs offline via `fake:*` models — same discipline as the rest of the
suite (no GPU, no keys, no network).
"""

from __future__ import annotations

import asyncio
import json

import httpx
import pytest

from mtrxai import __version__
from mtrxai.config import load_settings
from mtrxai.core.approval import Approver
from mtrxai.core.orchestrator import Orchestrator
from mtrxai.core.registry import Registry
from mtrxai.core.trace import Trace
from mtrxai.core.util import strip_code_fences  # noqa: F401  (import sanity)
from mtrxai.gui import theme
from mtrxai.gui.server import GuiServer, ServerState, serve  # noqa: F401
from mtrxai.gui.ui import WebUI
from mtrxai.tools import set_workspace

from fake import ScriptedBackend


# --------------------------------------------------------------------- palette

def test_palette_contract_holds_in_both_themes():
    """Every contracted pair meets its WCAG minimum — the palette's own promise."""
    failures = [(k, t, round(ratio, 2), minimum)
                for k, t, ratio, minimum in theme.contrast_report()
                if ratio < minimum]
    assert failures == [], f"contrast contract violations: {failures}"


def test_accent_anchor_is_exactly_the_requested_value():
    assert theme.ACCENT == "#26ab07"
    assert theme.ACCENT_RAMP["accent-600"] == "#26ab07"


def test_both_themes_define_the_same_tokens():
    """A token missing from one theme is a broken theme switch."""
    assert set(theme.LIGHT) == set(theme.DARK)
    assert set(theme.LIGHT) >= {"bg", "surface", "text", "accent", "accent-text",
                                "border", "success-text", "error-text", "warning-text"}


def test_theme_css_emits_both_blocks_and_the_ramp():
    css = theme.theme_css()
    assert ":root {" in css and "[data-theme='dark'] {" in css
    for token in ("--accent:", "--bg:", "--text:", "--accent-700:", "--accent-950:"):
        assert token in css, f"missing {token} in generated css"
    assert "#26ab07" in css


def test_semantic_status_colors_are_distinguishable():
    """success/error/warning/info must not collapse into near-identical hues."""
    for name, tokens in (("light", theme.LIGHT), ("dark", theme.DARK)):
        seen = [tokens[k] for k in ("success-text", "error-text", "warning-text", "info-text")]
        assert len(set(seen)) == 4, f"{name}: status colors collide: {seen}"


# ----------------------------------------------------------------------- WebUI

@pytest.mark.asyncio
async def test_webui_broadcasts_events_to_subscribers():
    ui = WebUI()
    ui.attach(asyncio.get_running_loop())
    q = ui.subscribe()
    ui.event({"type": "status", "text": "hi"})
    payload = await asyncio.wait_for(q.get(), timeout=1)
    assert payload == {"kind": "event", "evt": {"type": "status", "text": "hi"}}
    ui.unsubscribe(q)
    assert not ui.has_subscribers()


@pytest.mark.asyncio
async def test_webui_ask_resolves_through_resolve():
    ui = WebUI()
    ui.attach(asyncio.get_running_loop())
    q = ui.subscribe()
    task = asyncio.create_task(ui.ask("coder", "run_shell", {"command": "ls"}))
    card = await asyncio.wait_for(q.get(), timeout=1)
    assert card["kind"] == "card" and card["card"] == "approval"
    assert "run_shell" in card["title"]
    assert ui.resolve(card["id"], {"answer": "always"}) is True
    assert await asyncio.wait_for(task, timeout=1) == "always"
    # a second reply to the same card is a no-op, not a crash
    assert ui.resolve(card["id"], {"answer": "no"}) is False


@pytest.mark.asyncio
async def test_webui_confirm_agent_returns_edited_dict_and_cancel_is_none():
    ui = WebUI()
    ui.attach(asyncio.get_running_loop())
    q = ui.subscribe()

    t1 = asyncio.create_task(ui.confirm_agent({"name": "helper"}, "fake:demo"))
    c1 = await asyncio.wait_for(q.get(), timeout=1)
    assert c1["card"] == "agent_proposal" and c1["default_model"] == "fake:demo"
    ui.resolve(c1["id"], {"action": "create", "name": "helper", "description": "d",
                          "model": "fake:demo", "tools": ["read_file"]})
    assert (await asyncio.wait_for(t1, timeout=1))["tools"] == ["read_file"]

    t2 = asyncio.create_task(ui.confirm_agent({"name": "x"}, "fake:demo"))
    c2 = await asyncio.wait_for(q.get(), timeout=1)
    ui.resolve(c2["id"], {"action": "cancel"})
    assert await asyncio.wait_for(t2, timeout=1) is None


@pytest.mark.asyncio
async def test_webui_confirm_pull_and_disconnect_safety():
    ui = WebUI()
    ui.attach(asyncio.get_running_loop())
    q = ui.subscribe()
    t = asyncio.create_task(ui.confirm_pull("ollama:llama3.1:8b"))
    card = await asyncio.wait_for(q.get(), timeout=1)
    assert card["card"] == "confirm" and "llama3.1:8b" in card["title"]
    ui.resolve(card["id"], {"answer": "no"})
    assert await asyncio.wait_for(t, timeout=1) is False

    # a vanished browser must never leave an agent blocked on a card
    hung = asyncio.create_task(ui.ask("coder", "run_shell", {}))
    await asyncio.wait_for(q.get(), timeout=1)
    assert ui.pending_count() == 1
    assert ui.fail_pending() == 1
    assert await asyncio.wait_for(hung, timeout=1) == "no"  # safe direction: deny


# --------------------------------------------------------------- live server

def _entrypoint(proj):
    """A Registry/Orchestrator pair over ScriptedBackends rooted at `proj`."""
    proj.mkdir(parents=True, exist_ok=True)
    (proj / "agents").mkdir(exist_ok=True)
    (proj / "workspace").mkdir(exist_ok=True)
    (proj / "logs").mkdir(exist_ok=True)
    (proj / "settings.toml").write_text(
        'workspace = "./workspace"\nlogs = "./logs"\nagents_dir = "./agents"\n'
        'max_steps = 3\ndefault_permission = "ask"\nrouter_agent = "router"\n'
        'default_agent_model = "fake:demo"\n[providers.ollama]\n'
        'base_url = "http://127.0.0.1:1"\n'
    )
    (proj / "agents" / "router.yaml").write_text(
        "name: router\ndescription: Dispatcher.\nmodel: fake:demo\ntemperature: 0.0\n"
        "system_prompt: |\n  Route tasks.\n  {roster}\ntools: []\n"
    )
    (proj / "agents" / "coder.yaml").write_text(
        "name: coder\ndescription: Writes and runs code.\nmodel: fake:demo\ntemperature: 0.2\n"
        "system_prompt: |\n  You are Coder.\ntools: [run_shell]\n"
        "permissions:\n  run_shell: ask\nkeywords: [code, script]\n"
    )
    settings = load_settings(proj)
    set_workspace(settings["workspace"])
    backends: list[ScriptedBackend] = []

    def builder(ref, providers, gguf_cfg=None):
        b = ScriptedBackend()
        backends.append(b)
        return b

    reg = Registry(settings["agents_dir"], settings["providers"], {}, builder)
    ui = FakeWeb()
    trace = Trace(settings["logs"])
    approver = Approver(settings, ui,
                        lambda n: (reg.get(n).spec.permissions if reg.get(n) else {}))
    orch = Orchestrator(reg, settings, ui, approver, trace)
    return reg, orch, ui, backends


class FakeWeb(WebUI):
    """WebUI with auto-answer hooks so dispatch never blocks in tests."""

    def __init__(self):
        super().__init__()
        self.auto_answer = "yes"
        self.auto_proposal = None  # None = accept as proposed
        self.pull = False
        self.asked = 0
        self._sub: asyncio.Queue | None = None

    def subscribe(self):
        self._sub = super().subscribe()
        return self._sub

    async def ask(self, agent, tool, args):
        self.asked += 1
        self.event({"type": "tool_call", "agent": agent, "tool": tool, "args": args})
        return self.auto_answer

    async def confirm_agent(self, proposal, default_model):
        if self.auto_proposal is None:
            out = dict(proposal)
            out.setdefault("model", default_model)
            return out
        return self.auto_proposal

    async def confirm_pull(self, model_ref):
        return self.pull


@pytest.fixture
async def gui(tmp_path):
    """A real server on an ephemeral port + an httpx client against it."""
    import socket

    reg, orch, ui, backends = _entrypoint(tmp_path / "proj")
    ui.attach(asyncio.get_running_loop())

    class FakeRuntime:
        pass

    rt = FakeRuntime()
    rt.registry, rt.orchestrator, rt.settings, rt.trace = reg, orch, orch.settings, orch.trace
    state = ServerState(rt, ui)
    srv = GuiServer(state)

    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()

    await srv.start("127.0.0.1", port)
    transport = httpx.ASGITransport  # placeholder to keep import used
    del transport
    client = httpx.AsyncClient(base_url=f"http://127.0.0.1:{port}", timeout=10)
    try:
        yield client, srv, ui, reg, orch
    finally:
        await client.aclose()
        await srv.stop()


@pytest.mark.asyncio
async def test_index_and_assets_are_served(gui):
    client, *_ = gui
    r = await client.get("/")
    assert r.status_code == 200 and "mtrxAI" in r.text and "app.js" in r.text
    for path, needle in (("/theme.css", "#26ab07"),
                         ("/static/app.css", "--accent"),
                         ("/static/app.js", "/api/task")):
        rr = await client.get(path)
        assert rr.status_code == 200, path
        assert needle in rr.text, f"{needle} missing from {path}"


@pytest.mark.asyncio
async def test_state_reports_agents_and_router(gui):
    client, _, _, _, _ = gui
    s = (await client.get("/api/state")).json()
    names = {a["name"] for a in s["agents"]}
    assert {"router", "coder"} <= names
    assert s["router"] == "router"
    assert s["version"] == __version__
    assert [a for a in s["agents"] if a["router"]][0]["name"] == "router"


@pytest.mark.asyncio
async def test_full_dispatch_over_http_emits_routed_and_answer(gui):
    """The GUI's happy path: POST a task, see the same events the TUI shows."""
    client, _, ui, _, _ = gui
    sse: asyncio.Queue = asyncio.Queue()

    async def listen():
        async with client.stream("GET", "/api/events") as resp:
            async for line in resp.aiter_lines():
                if line.startswith("data: "):
                    await sse.put(json.loads(line[6:]))

    listener = asyncio.create_task(listen())
    # wait for the initial state frame so we know SSE is attached
    first = await asyncio.wait_for(sse.get(), timeout=5)
    assert first["kind"] == "state"

    r = await client.post("/api/task", json={"text": "write a haiku"})
    assert r.status_code == 200 and r.json()["ok"] is True

    seen, answer = [], None
    for _ in range(40):
        payload = await asyncio.wait_for(sse.get(), timeout=5)
        if payload["kind"] != "event":
            continue
        seen.append(payload["evt"]["type"])
        if payload["evt"]["type"] == "answer":
            answer = payload["evt"]["text"]
            break
    assert "routed" in seen, f"no routed event in {seen}"
    assert answer and "FINAL" in answer
    listener.cancel()


@pytest.mark.asyncio
async def test_tool_call_approval_round_trip_denies_when_told_no(gui):
    """A denied tool must come back as DENIED to the model, exactly like the CLI."""
    client, _, ui, reg, orch = gui
    ui.auto_answer = "no"
    reg.get("coder").backend.json_replies = ['{"agent": "coder", "proposal": null}']
    reg.get("coder").backend.replies = [
        '{"tool": "run_shell", "args": {"command": "echo hi"}}',
        "done without running it",
    ]
    r = await client.post("/api/task", json={"text": "run a shell command"})
    assert r.status_code == 200
    for _ in range(50):
        await asyncio.sleep(0.02)
        if ui.asked and not orch and False:
            break
        if ui.asked:
            break
    assert ui.asked == 1, "approval must be requested for run_shell (permission: ask)"
    # and the denial reached the model as a DENIED tool result
    msgs = reg.get("coder").backend.all_messages
    flat = json.dumps(msgs)
    assert "DENIED" in flat


@pytest.mark.asyncio
async def test_second_task_while_busy_is_rejected(gui):
    client, srv, ui, reg, _ = gui
    reg.get("coder").backend.json_replies = ['{"agent": "coder", "proposal": null}']

    async def slow_chat(messages, **kw):
        await asyncio.sleep(0.4)
        return "slow"

    reg.get("coder").backend.chat = slow_chat
    r1 = await client.post("/api/task", json={"text": "first"})
    assert r1.status_code == 200
    r2 = await client.post("/api/task", json={"text": "second"})
    assert r2.status_code == 409
    assert srv.st.busy() is True
    await asyncio.sleep(0.8)  # let the slow dispatch finish
    assert srv.st.busy() is False


@pytest.mark.asyncio
async def test_swap_persists_to_yaml_and_reload_and_trace(gui):
    client, _, _, reg, _ = gui
    r = await client.post("/api/swap", json={"agent": "coder", "ref": "fake:tools"})
    assert r.status_code == 200, r.text
    assert "fake:tools" in r.json()["message"]
    assert "model: fake:tools" in (reg.dir / "coder.yaml").read_text()

    bad = await client.post("/api/swap", json={"agent": "nope", "ref": "fake:demo"})
    assert bad.status_code == 400 and "swap failed" in bad.json()["error"]

    rl = await client.post("/api/reload")
    assert rl.status_code == 200 and rl.json()["ok"] is True

    tr = await client.get("/api/trace")
    assert tr.status_code == 200 and "trace" in tr.json()


@pytest.mark.asyncio
async def test_slash_commands_over_http_match_the_repl(gui):
    client, *_ = gui
    agents = await client.post("/api/task", json={"text": "/agents"})
    assert agents.status_code == 200
    assert {a["name"] for a in agents.json()["agents"]} >= {"router", "coder"}

    helptext = await client.post("/api/task", json={"text": "/help"})
    assert helptext.status_code == 200
    assert any("/swap" in str(x) for x in helptext.json()["help"])

    bad = await client.post("/api/task", json={"text": "/nonsense"})
    assert bad.status_code == 400

    swap = await client.post("/api/task", json={"text": "/swap coder fake:demo"})
    assert swap.status_code == 200 and "coder" in swap.json()["message"]


@pytest.mark.asyncio
async def test_unknown_route_and_oversized_payload(gui):
    client, *_ = gui
    assert (await client.get("/nope")).status_code == 404
    assert (await client.get("/favicon.ico")).status_code == 204
    big = await client.post("/api/task", json={"text": "x" * (70 * 1024)})
    assert big.status_code == 413
