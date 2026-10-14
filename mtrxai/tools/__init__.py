"""Importing this package registers every built-in tool in TOOLBOX."""

from .base import TOOLBOX, Tool, Toolbox, set_workspace, tool, workspace
from . import files, shell, web  # noqa: F401 — side effect: registration

__all__ = ["TOOLBOX", "Tool", "Toolbox", "tool", "set_workspace", "workspace"]
