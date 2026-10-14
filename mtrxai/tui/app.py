"""The mtrxAI Textual TUI: chat log + live agent sidebar + status bar + voice.

Concurrency model (the part that matters):
  - agent runs are workers, so input never freezes
  - the approval modal awaits a future; the tool loop blocks on it, nothing else does
  - the voice service is a plain thread talking to the UI via call_from_thread
"""

from __future__ import annotations

import asyncio
import threading
from pathlib import Path

from rich.markup import escape
from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header, Input, RichLog, Static

from .. import __version__
from ..core.runtime import assemble
from .modals import AgentProposalModal, ApprovalModal, ConfirmModal, SwapModal


class TuiUI:
    """Implements the core UI protocol on top of Textual widgets."""

    def __init__(self, app: "MtrxApp"):
        self.app = app

    def _on_app_thread(self) -> bool:
        ident = getattr(self.app, "_mtrx_thread_id", None)
        return ident is not None and threading.get_ident() == ident

    def event(self, evt: dict) -> None:
        if self._on_app_thread():
            self.app.call_next(self.app.render_event, evt)
        else:
            self.app.call_from_thread(self.app.render_event, evt)

    async def ask(self, agent: str, tool: str, args: dict) -> str:
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self.app.push_screen(ApprovalModal(agent, tool, args), callback=fut.set_result)
        return await fut

    async def confirm_agent(self, proposal: dict, default_model: str) -> dict | None:
        fut: asyncio.Future = asyncio.get_running_loop().create_future()

        def _done(result: dict | None) -> None:
            if not fut.done():
                fut.set_result(result)

        self.app.push_screen(
            AgentProposalModal(proposal, default_model), callback=_done
        )
        return await fut

    async def confirm_pull(self, model_ref: str) -> bool:
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        model = model_ref.partition(":")[2]
        self.app.push_screen(
            ConfirmModal(f"model '{model}' is not in Ollama.\nPull it now?"),
            callback=fut.set_result,
        )
        return await fut


class MtrxApp(App):
    TITLE = f"mtrxAI v{__version__}"
    SUB_TITLE = "hot-swappable agents"

    BINDINGS = [
        Binding("ctrl+q", "quit", "Quit"),
        Binding("ctrl+r", "reload", "Reload agents"),
        Binding("ctrl+s", "swap", "Swap model"),
        Binding("ctrl+v", "toggle_voice", "Voice on/off"),
        Binding("ctrl+t", "trace", "Last trace"),
        Binding("escape", "barge_in", "Stop speech", show=False),
    ]

    CSS = """
    #body { height: 1fr; }
    #chat { width: 1fr; }
    #log { height: 1fr; border: round $primary; padding: 0 1; }
    #task { dock: bottom; }
    #sidebar { width: 34; border: round $accent; padding: 1 1 0 1; }
    #status { height: 1; color: $text-muted; }
    #modal-box { width: 60; max-height: 80%; border: thick $accent; padding: 1 2;
                 background: $surface; }
    #modal-title { margin-bottom: 1; }
    #modal-detail { margin-bottom: 1; color: $text-muted; }
    #modal-hint { color: $text-muted; margin-top: 1; }
    #modal-buttons { height: auto; align: center middle; margin-top: 1; }
    #modal-buttons Button { margin: 0 2; }
    """

    def __init__(self, project_root: Path | None = None, voice: bool = False):
        super().__init__()
        self.project_root = project_root
        self.voice_requested = voice
        self.rt = None
        self.voice_service = None
        self._tts = None
        self._speech_stop = threading.Event()
        self._busy = False
        self._voice_state = "off"

    # ------------------------------------------------------------- composition

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal(id="body"):
            with Vertical(id="chat"):
                yield RichLog(id="log", markup=True, wrap=True, max_lines=5000)
                yield Input(placeholder="task… (or say “hey matrix”)", id="task")
            yield Static("", id="sidebar", markup=True)
        yield Static("", id="status", markup=True)
        yield Footer()

    def on_mount(self) -> None:
        self._mtrx_thread_id = threading.get_ident()
        ui = TuiUI(self)
        self.rt = assemble(self.project_root, ui)
        self.log_line(f"[bold magenta]mtrxAI[/bold magenta] "
                      f"[dim]v{__version__} · {len(self.rt.registry.names())} agents · "
                      f"{escape(str(self.rt.settings['project_root']))}[/dim]")
        for w in self.rt.registry.warnings:
            self.log_line(f"[yellow]⚠ {escape(w)}[/yellow]")
        self.refresh_sidebar()
        self.refresh_status()
        if self.voice_requested or self.rt.settings["voice"].get("enabled"):
            self.start_voice()
        self.query_one("#task", Input).focus()

    # ------------------------------------------------------------- UI helpers

    def log_line(self, markup: str) -> None:
        self.query_one("#log", RichLog).write(markup)

    def refresh_sidebar(self) -> None:
        lines = ["[b]AGENTS[/b]", ""]
        router = self.rt.orchestrator.router_name
        for la in self.rt.registry.all():
            tag = " [dim]· router[/dim]" if la.spec.name == router else ""
            lines.append(f"[cyan]●[/cyan] [b]{escape(la.spec.name)}[/b]{tag}")
            lines.append(f"  [dim]{escape(la.spec.model)}[/dim]")
            lines.append("")
        v = self.rt.settings["voice"]
        lines += ["[b]VOICE[/b]", f"[dim]{escape(str(v['stt']))}[/dim]",
                  f"[dim]{escape(str(v['tts']))}[/dim]"]
        self.query_one("#sidebar", Static).update("\n".join(lines))

    def refresh_status(self, busy: bool | None = None) -> None:
        if busy is not None:
            self._busy = busy
        left = "⟳ working…" if self._busy else "idle"
        try:
            status = self.query_one("#status", Static)
        except Exception:
            return  # widgets already torn down (unmount)
        status.update(f"{left}   ·   voice: {escape(self._voice_state)}")

    def set_voice_status(self, state: str) -> None:
        self._voice_state = state
        self.refresh_status()
    def render_event(self, evt: dict) -> None:
        t = evt.get("type", "")
        if t == "routed":
            self.log_line(f"[cyan]⚡ routed →[/cyan] [b]{escape(evt['agent'])}[/b] "
                          f"[dim]({escape(evt.get('model', ''))})[/dim]")
        elif t == "routing_fallback":
            self.log_line(f"[yellow]route fallback: {escape(str(evt.get('reason', ''))[:80])}[/yellow]")
        elif t == "proposal":
            self.log_line("[magenta]✦ router proposes a new agent[/magenta]")
        elif t == "agent_created":
            self.log_line(f"[green]✓ agent '{escape(evt['name'])}' created[/green]")
            self.refresh_sidebar()
        elif t == "agent_declined":
            self.log_line("[yellow]agent creation declined[/yellow]")
        elif t == "tool_call":
            self.log_line(f"[b]🔧 {escape(evt['agent'])} → {escape(evt['tool'])}[/b] "
                          f"[dim]{escape(str(evt.get('args', {}))[:200])}[/dim]")
        elif t == "tool_denied":
            self.log_line(f"[red]⊘ {escape(evt['agent'])} · {escape(evt['tool'])} denied[/red]")
        elif t == "tool_result":
            self.log_line(f"[dim]↳ {escape(str(evt.get('result', ''))[:200])}[/dim]")
        elif t == "pull":
            self.log_line(f"[dim]⇣ {escape(str(evt.get('line', ''))[:100])}[/dim]")
        elif t == "status":
            self.log_line(f"[dim italic]{escape(str(evt.get('text', '')))}[/dim italic]")
        elif t == "error":
            self.log_line(f"[red]⚠ {escape(str(evt.get('text', ''))[:300])}[/red]")

    # ------------------------------------------------------------- task dispatch

    @on(Input.Submitted, "#task")
    def on_task(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.clear()
        if not text:
            return
        if text.startswith("/"):
            self.slash(text)
        else:
            self.log_line(f"[dim]you ›[/dim] {escape(text)}")
            self.run_worker(self._run_task(text, source="text"), exclusive=False)

    async def _run_task(self, text: str, source: str = "text") -> None:
        self.refresh_status(busy=True)
        try:
            answer = await self.rt.orchestrator.dispatch(text, source=source)
        except Exception as e:  # noqa: BLE001 — surface, never crash the app
            self.log_line(f"[red]⚠ dispatch failed: {escape(str(e)[:300])}[/red]")
            return
        finally:
            self.refresh_status(busy=False)
        self.log_line(f"[bold cyan]mtrxAI ›[/bold cyan] {escape(answer)}\n")
        if source == "voice" and self.rt.settings["voice"].get("speak_responses"):
            self._speech_stop.clear()
            await asyncio.to_thread(self._speak, answer)

    def handle_voice_text(self, text: str) -> None:
        """Called (from the voice thread, via call_from_thread) with an utterance."""
        self.log_line(f"[magenta]🎙 {escape(text)}[/magenta]")
        if self.rt.settings["voice"].get("auto_submit"):
            self.run_worker(self._run_task(text, source="voice"), exclusive=False)
        else:
            self.query_one("#task", Input).value = text

    # ------------------------------------------------------------- slash commands

    def slash(self, text: str) -> None:
        cmd, _, rest = text.partition(" ")
        cmd = cmd.lower()
        registry, orch = self.rt.registry, self.rt.orchestrator
        if cmd in ("/quit", "/exit", "/q"):
            self.exit()
        elif cmd in ("/help", "/?"):
            self.log_line("[dim]/agents · /swap · /reload · /create [name] · /trace · "
                          "/voice · /quit — or ctrl+s ctrl+r ctrl+v ctrl+t[/dim]")
        elif cmd == "/agents":
            for la in registry.all():
                tag = " [dim]· router[/dim]" if la.spec.name == orch.router_name else ""
                self.log_line(f"[cyan]●[/cyan] [b]{escape(la.spec.name)}[/b]{tag} — "
                              f"[dim]{escape(la.spec.model)} · "
                              f"{escape(', '.join(la.spec.tools) or 'no tools')}[/dim]")
        elif cmd == "/swap":
            self.action_swap()
        elif cmd == "/reload":
            registry.load()
            self.refresh_sidebar()
            self.log_line(f"[green]✓ {len(registry.names())} agents loaded[/green]")
            for w in registry.warnings:
                self.log_line(f"[yellow]⚠ {escape(w)}[/yellow]")
        elif cmd == "/create":
            self._open_create(rest.strip())
        elif cmd == "/trace":
            self.log_line(escape(self.rt.trace.format_last()))
        elif cmd == "/voice":
            self.action_toggle_voice()
        else:
            self.log_line(f"[yellow]unknown command {escape(cmd)} — /help[/yellow]")

    def _open_create(self, name: str) -> None:
        proposal = {"name": name} if name else {}
        self.push_screen(
            AgentProposalModal(proposal, self.rt.orchestrator.default_model),
            callback=self._create_callback,
        )

    def _create_callback(self, result: dict | None) -> None:
        if result is None:
            return
        self.run_worker(self._create_agent(result), exclusive=False)

    async def _create_agent(self, proposal: dict) -> None:
        la = await self.rt.orchestrator.create_flow(proposal)
        if la is not None:
            self.refresh_sidebar()

    # ------------------------------------------------------------- bindings

    def action_reload(self) -> None:
        self.rt.registry.load()
        self.refresh_sidebar()
        self.log_line(f"[green]✓ reloaded {len(self.rt.registry.names())} agents[/green]")

    def action_swap(self) -> None:
        router = self.rt.orchestrator.router_name
        agents = [(la.spec.name, la.spec.model) for la in self.rt.registry.all()
                  if la.spec.name != router]
        if not agents:
            self.log_line("[yellow]no swappable agents[/yellow]")
            return
        self.push_screen(SwapModal(agents), callback=self._swap_callback)

    def _swap_callback(self, result: tuple[str, str] | None) -> None:
        if result is None:
            return
        agent, ref = result
        try:
            la = self.rt.registry.swap(agent, ref)
        except Exception as e:  # noqa: BLE001
            self.notify(f"swap failed: {e}", severity="error")
            return
        self.refresh_sidebar()
        self.log_line(f"[green]✓ {escape(la.spec.name)} → {escape(la.spec.model)}[/green]")

    def action_trace(self) -> None:
        self.log_line(escape(self.rt.trace.format_last()))

    def action_barge_in(self) -> None:
        self._speech_stop.set()
        try:
            import sounddevice as sd

            sd.stop()
        except Exception:
            pass

    # ------------------------------------------------------------- voice

    def action_toggle_voice(self) -> None:
        if self.voice_service is not None and self.voice_service.is_alive():
            self.stop_voice()
        else:
            self.start_voice()

    def start_voice(self) -> None:
        from ..voice.pipeline import VoiceService

        cfg = self.rt.settings["voice"]
        hooks = {
            "status": lambda s: self.call_from_thread(self.set_voice_status, s),
            "text": lambda t: self.call_from_thread(self.handle_voice_text, t),
        }
        self.voice_service = VoiceService(cfg, hooks)
        self.voice_service.start()
        self.refresh_sidebar()

    def stop_voice(self) -> None:
        if self.voice_service is not None:
            self.voice_service.stop()
            self.voice_service = None
        self._voice_state = "off"
        self.call_next(self.refresh_status)  # widgets may be gone during unmount

    def _speak(self, text: str) -> None:
        from ..voice.tts import make_tts

        if self._tts is None:
            self._tts = make_tts(self.rt.settings["voice"]["tts"],
                                 self.rt.settings["voice"]["voices_dir"])
        try:
            self._tts.speak(text, stop=self._speech_stop)
        except Exception as e:  # noqa: BLE001
            self.call_from_thread(self.log_line, f"[yellow]tts: {escape(str(e)[:200])}[/yellow]")

    def on_unmount(self) -> None:
        self.stop_voice()


def run_tui(project_root: Path | None = None, voice: bool = False) -> int:
    app = MtrxApp(project_root, voice)
    app.run()
    return 0
