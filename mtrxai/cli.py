"""mtrxai entry point: TUI by default, REPL as the debugging baseline."""

from __future__ import annotations

import argparse
import asyncio
import sys

from rich.console import Console
from rich.markup import escape
from rich.table import Table

from . import __version__
from .config import find_project_root
from .core.runtime import assemble
from .core.ui import ConsoleUI


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="mtrxai",
        description="Hot-swappable multi-agent assistant (router + specialists).",
    )
    parser.add_argument(
        "--mode", choices=["tui", "repl", "web"], default="tui", help="frontend (default: tui)"
    )
    parser.add_argument("--voice", action="store_true", help="arm the 'hey matrix' wake word (TUI)")
    parser.add_argument("--project", metavar="DIR", help="project root with settings.toml")
    parser.add_argument("--host", default="127.0.0.1", help="web GUI bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8417, help="web GUI port (default: 8417)")
    parser.add_argument("--version", action="version", version=f"mtrxai {__version__}")
    args = parser.parse_args(argv)

    root = find_project_root(args.project)

    if args.mode == "tui":
        try:
            from .tui.app import run_tui
        except ImportError:
            print("Textual is not installed — falling back to the REPL. (pip install -e '.[tui]')")
            args.mode = "repl"
        else:
            return run_tui(root, voice=args.voice)

    if args.mode == "web":
        return asyncio.run(_web(root, host=args.host, port=args.port))

    return asyncio.run(_repl(root))


# ----------------------------------------------------------------------------- web


async def _web(root, host: str, port: int) -> int:
    from .gui.server import serve
    from .gui.ui import WebUI
    from .core.runtime import assemble

    ui = WebUI()
    rt = assemble(root, ui)
    url = f"http://{host}:{port}"
    print(f"mtrxAI v{__version__} · web GUI on {url} · agents: {len(rt.registry.names())}")
    for w in rt.registry.warnings:
        print(f"  ⚠ {w}")
    print("ctrl+c to stop")
    try:
        await serve(rt, ui, host, port)
    except KeyboardInterrupt:
        pass
    return 0


# --------------------------------------------------------------------------- REPL


async def _repl(root) -> int:
    console = Console()
    ui = ConsoleUI(console)
    rt = assemble(root, ui)
    orch, registry = rt.orchestrator, rt.registry

    console.print(
        f"[bold magenta]mtrxAI[/bold magenta] [dim]v{__version__} · {root}[/dim]"
    )
    console.print(
        f"[dim]{len(registry.names())} agents loaded · workspace: {rt.settings['workspace']}[/dim]"
    )
    console.print("[dim]/help for commands · /quit to exit[/dim]")
    for w in registry.warnings:
        console.print(f"[yellow]⚠ {escape(w)}[/yellow]")

    while True:
        try:
            text = console.input("[bold green]you ›[/] ").strip()
        except (EOFError, KeyboardInterrupt):
            console.print("\n[dim]bye[/dim]")
            return 0
        if not text:
            continue

        if text.startswith("/"):
            await _command(text, console, rt)
            continue

        try:
            answer = await orch.dispatch(text)
        except KeyboardInterrupt:
            console.print("[yellow](interrupted)[/yellow]")
            continue
        console.print(f"[bold cyan]mtrxAI ›[/bold cyan] {escape(answer)}\n")


async def _command(text: str, console: Console, rt) -> None:
    orch, registry = rt.orchestrator, rt.registry
    cmd, _, rest = text.partition(" ")
    cmd = cmd.lower()

    if cmd in ("/quit", "/exit", "/q"):
        raise SystemExit(0)
    elif cmd in ("/help", "/?"):
        table = Table(title="commands", border_style="dim", show_header=False)
        table.add_column(style="bold")
        table.add_column()
        for row in [
            ("(just type)", "route + run a task"),
            ("/agents", "list agents with their current model refs"),
            ("/swap <agent> <provider:model>", "hot-swap one agent's model now"),
            ("/reload", "re-read all agent YAMLs from disk"),
            ("/create [name]", "manual agent-creation wizard"),
            ("/trace", "print the last run's routing/tool trace"),
            ("/quit", "exit"),
        ]:
            table.add_row(*row)
        console.print(table)
    elif cmd == "/agents":
        table = Table(title="agents", border_style="dim")
        table.add_column("name", style="bold")
        table.add_column("model", style="cyan")
        table.add_column("tools", style="dim")
        table.add_column("description")
        for la in registry.all():
            tag = " [dim](router)[/dim]" if la.spec.name == orch.router_name else ""
            table.add_row(
                escape(la.spec.name) + tag,
                escape(la.spec.model),
                escape(", ".join(la.spec.tools) or "—"),
                escape(la.spec.description),
            )
        console.print(table)
    elif cmd == "/swap":
        parts = rest.split()
        if len(parts) != 2:
            console.print("[yellow]usage: /swap <agent> <provider:model>[/yellow]")
        else:
            try:
                la = registry.swap(parts[0], parts[1])
                console.print(f"[green]✓ {la.spec.name} → {la.spec.model}[/green]")
            except Exception as e:  # noqa: BLE001 — user-facing swap errors
                console.print(f"[red]swap failed: {escape(str(e))}[/red]")
    elif cmd == "/reload":
        registry.load()
        console.print(f"[green]✓ {len(registry.names())} agents loaded[/green]")
        for w in registry.warnings:
            console.print(f"[yellow]⚠ {escape(w)}[/yellow]")
    elif cmd == "/create":
        name = rest.strip()
        proposal = {"name": name} if name else {}
        la = await orch.create_flow(proposal)
        if la is not None:
            console.print(f"[green]dispatch a task to it, or /swap {la.spec.name} <model>[/green]")
    elif cmd == "/trace":
        console.print(rt.trace.format_last())
    else:
        console.print(f"[yellow]unknown command {escape(cmd)} — /help[/yellow]")


if __name__ == "__main__":
    sys.exit(main())
