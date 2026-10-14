"""TUI modals: approval, generic confirm, hot-swap, and the new-agent proposal card."""

from __future__ import annotations

import re
from typing import Any

from rich.markup import escape
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Select, Static

_KEBAB = re.compile(r"^[a-z0-9][a-z0-9-]*$")


class ApprovalModal(ModalScreen[str]):
    """y / a / n — returns 'yes' | 'always' | 'no'."""

    BINDINGS = [
        Binding("y", "yes", "allow"),
        Binding("a", "always", "always (session)"),
        Binding("n", "no", "deny"),
        Binding("escape", "no", "deny", show=False),
    ]

    def __init__(self, agent: str, tool: str, args: dict) -> None:
        super().__init__()
        self.agent, self.tool, self.args = agent, tool, args

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-box"):
            yield Static(f"[b]approval[/b]\n\n[b]{escape(self.agent)}[/b] wants to run "
                         f"[b]{escape(self.tool)}[/b]", id="modal-title")
            yield Static(escape(str(self.args))[:600], id="modal-detail")
            yield Static("[b]y[/b] allow · [b]a[/b] always this session · [b]n[/b] deny",
                         id="modal-hint")

    def action_yes(self) -> None:
        self.dismiss("yes")

    def action_always(self) -> None:
        self.dismiss("always")

    def action_no(self) -> None:
        self.dismiss("no")


class ConfirmModal(ModalScreen[bool]):
    BINDINGS = [
        Binding("y", "yes", "yes"),
        Binding("n", "no", "no"),
        Binding("escape", "no", "no", show=False),
    ]

    def __init__(self, message: str) -> None:
        super().__init__()
        self.message = message

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-box"):
            yield Static(self.message, id="modal-title")
            yield Static("[b]y[/b] yes · [b]n[/b] no", id="modal-hint")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)


class SwapModal(ModalScreen[tuple[str, str] | None]):
    """Pick an agent, edit its provider:model reference, Enter to swap."""

    BINDINGS = [Binding("escape", "cancel", "cancel", show=False)]

    def __init__(self, agents: list[tuple[str, str]]) -> None:
        super().__init__()
        # agents: [(name, current model ref)]
        self.agents = agents
        self.current: dict[str, str] = dict(agents)

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-box"):
            yield Static("[b]hot-swap model[/b]", id="modal-title")
            yield Select(
                [(name, name) for name, _ in self.agents],
                prompt="agent",
                id="swap-agent",
                allow_blank=False,
            )
            yield Input(placeholder="provider:model  (e.g. openai:gpt-4o)", id="swap-model")
            yield Static("Enter to swap · Esc to cancel", id="modal-hint")

    def on_mount(self) -> None:
        if self.agents:
            select = self.query_one("#swap-agent", Select)
            select.value = self.agents[0][0]
            self.query_one("#swap-model", Input).value = self.agents[0][1]

    def on_select_changed(self, event: Select.Changed) -> None:
        if event.value in self.current:
            self.query_one("#swap-model", Input).value = self.current[event.value]

    def on_input_submitted(self, event: Input.Submitted) -> None:
        agent = self.query_one("#swap-agent", Select).value
        ref = event.value.strip()
        if agent and ref:
            self.dismiss((str(agent), ref))

    def action_cancel(self) -> None:
        self.dismiss(None)


class AgentProposalModal(ModalScreen[dict | None]):
    """Approval card for a proposed new agent — edit anything, then Create/Cancel."""

    BINDINGS = [
        Binding("escape", "cancel", "cancel", show=False),
        Binding("ctrl+enter", "submit", "create"),
    ]

    def __init__(self, proposal: dict, default_model: str) -> None:
        super().__init__()
        self.proposal = proposal
        self.default_model = default_model

    def compose(self) -> ComposeResult:
        with Vertical(id="modal-box"):
            yield Static("[b]proposed new agent[/b]", id="modal-title")
            yield Input(value=str(self.proposal.get("name", "")), placeholder="name (kebab-case)",
                        id="np-name")
            yield Input(value=str(self.proposal.get("description", "")),
                        placeholder="one-line description", id="np-desc")
            yield Input(value=str(self.default_model), placeholder="model ref provider:model",
                        id="np-model")
            yield Input(value=", ".join(self.proposal.get("tools") or []),
                        placeholder="tools, comma-separated", id="np-tools")
            with Horizontal(id="modal-buttons"):
                yield Button("Create", variant="success", id="np-create")
                yield Button("Cancel", variant="error", id="np-cancel")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "np-create":
            self.action_submit()
        else:
            self.action_cancel()

    def _collect(self) -> dict[str, Any]:
        tools = [
            t.strip()
            for t in self.query_one("#np-tools", Input).value.split(",")
            if t.strip()
        ]
        return {
            "name": self.query_one("#np-name", Input).value.strip(),
            "description": self.query_one("#np-desc", Input).value.strip(),
            "model": self.query_one("#np-model", Input).value.strip(),
            "tools": tools,
        }

    def action_submit(self) -> None:
        data = self._collect()
        if not _KEBAB.match(data["name"]):
            self.app.notify("name must be kebab-case, e.g. data-analyst", severity="error")
            return
        self.dismiss(data)

    def action_cancel(self) -> None:
        self.dismiss(None)
