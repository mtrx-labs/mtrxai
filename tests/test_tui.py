"""Headless TUI smoke test via Textual's pilot — no terminal needed."""

import pytest

pytest.importorskip("textual", reason="textual not installed")

from mtrxai.tui.app import MtrxApp  # noqa: E402


@pytest.mark.asyncio
async def test_tui_dispatch_end_to_end(proj):
    app = MtrxApp(project_root=proj)
    events: list[dict] = []
    orig = app.render_event
    app.render_event = lambda evt: (events.append(evt), orig(evt))

    async with app.run_test(size=(100, 30)) as pilot:
        from textual.widgets import Input

        task = app.query_one("#task", Input)
        task.value = "hello there"
        await pilot.press("enter")
        await app.workers.wait_for_complete()
        await pilot.pause()

    assert any(e["type"] == "routed" and e["agent"] == "writer" for e in events)
    assert any(e["type"] == "answer" and "fake" in e["text"] for e in events)


@pytest.mark.asyncio
async def test_tui_slash_agents_and_reload(proj):
    app = MtrxApp(project_root=proj)
    async with app.run_test(size=(100, 30)) as pilot:
        from textual.widgets import Input, RichLog

        task = app.query_one("#task", Input)
        task.value = "/agents"
        await pilot.press("enter")
        await pilot.pause()
        task.value = "/reload"
        await pilot.press("enter")
        await pilot.pause()
        task.value = "/trace"
        await pilot.press("enter")
        await pilot.pause()
        log_text = "\n".join(
            seg.text for line in app.query_one("#log", RichLog).lines for seg in line
        )
        assert "writer" in log_text
        assert "reloaded" in log_text or "agents loaded" in log_text
