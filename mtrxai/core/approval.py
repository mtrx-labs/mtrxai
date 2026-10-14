"""The permission gate. Both dangerous tool use and new-agent creation pass through it.

Policy precedence for a tool call:
    agent spec permissions > the @tool decorator's allow/deny > settings
    default_permission. A decorator default of "ask" is not a hard policy — it
defers to settings.default_permission.
"""

from __future__ import annotations

from typing import Callable, Protocol

from ..tools.base import TOOLBOX

VALID = ("allow", "ask", "deny")


class ApprovalUI(Protocol):
    async def ask(self, agent: str, tool: str, args: dict) -> str:
        """Show a prompt; return 'yes' | 'always' | 'no'."""
        ...


class Approver:
    def __init__(self, settings: dict, ui: ApprovalUI, permissions_of: Callable[[str], dict]):
        self.default = settings.get("default_permission", "ask")
        if self.default not in VALID:
            self.default = "ask"
        self.ui = ui
        self.permissions_of = permissions_of  # agent name → its permissions dict
        self.session_ok: set[str] = set()  # {"coder:run_shell", …} after "always"

    def policy(self, agent: str, tool_name: str) -> str:
        """spec permissions > the @tool decorator's allow/deny > settings default.

        A decorator default of "ask" defers to settings.default_permission, so the
        global setting can tighten "ask" to "deny" (or loosen it to "allow").
        """
        t = TOOLBOX.get(tool_name)
        spec_perm = (self.permissions_of(agent) or {}).get(tool_name)
        if spec_perm in VALID:
            return spec_perm
        if t is not None and t.permission in ("allow", "deny"):
            return t.permission
        return self.default

    async def check(self, agent: str, call: dict) -> bool:
        name = call.get("tool")
        if name not in TOOLBOX:
            return False  # unknown tools never execute (the Toolbox returns an ERROR string)
        mode = self.policy(agent, name)
        key = f"{agent}:{name}"
        if mode == "allow" or key in self.session_ok:
            return True
        if mode == "deny":
            return False
        ans = await self.ui.ask(agent, name, call.get("args") or {})
        if ans == "always":
            self.session_ok.add(key)
            return True
        return ans == "yes"

    def forget_session_grants(self) -> None:
        self.session_ok.clear()
