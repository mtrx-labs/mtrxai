"""UI-agnostic frontend contract + the rich Console implementation used by the REPL.

The TUI injects its own implementation of the same protocol; the orchestrator,
agents and approval gate never know which frontend is attached.
"""

from __future__ import annotations

import json
from typing import Protocol

from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.prompt import Confirm, Prompt
from rich.table import Table


class UI(Protocol):
    def event(self, evt: dict) -> None:
        """Progress notification: routed / tool_call / tool_result / status / error…"""

    async def ask(self, agent: str, tool: str, args: dict) -> str:
        """Approval prompt → 'yes' | 'always' | 'no'."""

    async def confirm_agent(self, proposal: dict, default_model: str) -> dict | None:
        """Approval card for a proposed new agent; may be edited; None = declined."""

    async def confirm_pull(self, model_ref: str) -> bool:
        """Ask whether to `ollama pull` a missing model."""


def _short(s: str, n: int = 160) -> str:
    s = str(s).replace("\n", " ⏎ ")
    return s if len(s) <= n else s[: n - 1] + "…"


class ConsoleUI:
    def __init__(self, console: Console):
        self.console = console

    def event(self, evt: dict) -> None:
        t = evt.get("type", "")
        if t == "routed":
            self.console.print(
                f"  [cyan]⚡ routed →[/cyan] [bold]{escape(evt['agent'])}[/bold] "
                f"[dim]({escape(evt.get('model', ''))})[/dim]"
            )
        elif t == "routing_fallback":
            self.console.print(
                f"  [yellow]route fallback ({escape(_short(evt.get('reason', ''), 80))})[/yellow]"
            )
        elif t == "proposal":
            self.console.print("  [magenta]✦ router proposes a new agent[/magenta]")
        elif t == "agent_created":
            self.console.print(
                f"  [green]✓ agent '{escape(evt['name'])}' created ({escape(evt.get('model', ''))})[/green]"
            )
        elif t == "agent_declined":
            self.console.print("  [yellow]agent creation declined[/yellow]")
        elif t == "tool_call":
            args = _short(json.dumps(evt.get("args", {}), ensure_ascii=False), 200)
            self.console.print(
                f"  [bold]🔧 {escape(evt['agent'])} → {escape(evt['tool'])}[/bold] "
                f"[dim]{escape(args)}[/dim]"
            )
        elif t == "tool_denied":
            self.console.print(f"  [red]⊘ {escape(evt['agent'])} · {escape(evt['tool'])} denied[/red]")
        elif t == "tool_result":
            self.console.print(f"  [dim]↳ {escape(_short(evt.get('result', ''), 220))}[/dim]")
        elif t == "pull":
            self.console.print(f"  [dim]⇣ {escape(_short(evt.get('line', ''), 100))}[/dim]")
        elif t == "status":
            self.console.print(f"  [dim italic]{escape(evt.get('text', ''))}[/dim italic]")
        elif t == "error":
            self.console.print(f"  [red]⚠ {escape(_short(evt.get('text', ''), 300))}[/red]")

    async def ask(self, agent: str, tool: str, args: dict) -> str:
        pretty = _short(json.dumps(args, ensure_ascii=False), 300)
        self.console.print(
            Panel.fit(
                f"[bold]{escape(agent)}[/bold] wants to run [bold]{escape(tool)}[/bold]\n"
                f"[dim]{escape(pretty)}[/dim]",
                title="[bold]approval[/bold]",
                border_style="yellow",
            )
        )
        ans = Prompt.ask(
            "  allow?",
            choices=["y", "a", "n"],
            default="n",
            show_choices=True,
        )
        return {"y": "yes", "a": "always"}.get(ans, "no")

    async def confirm_agent(self, proposal: dict, default_model: str) -> dict | None:
        table = Table(title="proposed new agent", border_style="magenta", show_header=False)
        table.add_column(style="bold")
        table.add_column()
        table.add_row("name", proposal.get("name", ""))
        table.add_row("description", proposal.get("description", ""))
        table.add_row("tools", ", ".join(proposal.get("tools") or []) or "(none)")
        table.add_row("model", default_model)
        self.console.print(table)
        ans = Prompt.ask(
            "  create this agent? ([y]es · [e]dit · [n]o)",
            choices=["y", "e", "n"],
            default="y",
        )
        if ans == "n":
            return None
        out = dict(proposal)
        if ans == "e":
            out["model"] = Prompt.ask("  model ref", default=default_model)
            out["tools"] = [
                t.strip()
                for t in Prompt.ask(
                    "  tools (comma-separated)", default=", ".join(out.get("tools") or [])
                ).split(",")
                if t.strip()
            ]
        return out

    async def confirm_pull(self, model_ref: str) -> bool:
        model = model_ref.partition(":")[2]
        return Confirm.ask(f"  model '{model}' is not in Ollama — pull it now?", default=True)
