"""File tools, jailed to the workspace directory."""

from __future__ import annotations

from pathlib import Path

from .base import tool, workspace

MAX_READ_CHARS = 24_000


def safe_path(rel: str) -> Path:
    """Resolve `rel` inside the workspace; reject escapes."""
    root = workspace()
    p = (root / rel).resolve() if rel else root
    if not p.is_relative_to(root):
        raise PermissionError(f"path '{rel}' escapes the workspace jail")
    return p


def _rel(p: Path) -> str:
    try:
        return str(p.relative_to(workspace()))
    except ValueError:
        return str(p)


@tool(
    "read_file",
    "Read a text file from the workspace.",
    {"path": "string"},
    permission="allow",
)
def read_file(args: dict) -> str:
    p = safe_path(args["path"])
    try:
        text = p.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, PermissionError) as e:
        return f"ERROR: {type(e).__name__}: {e}"
    if len(text) > MAX_READ_CHARS:
        text = text[:MAX_READ_CHARS] + f"\n… truncated ({len(text)} chars total)"
    return f"[{_rel(p)}]\n{text}"


@tool(
    "write_file",
    "Create or overwrite a file in the workspace.",
    {"path": "string", "content": "string"},
    permission="allow",
)
def write_file(args: dict) -> str:
    p = safe_path(args["path"])
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(args["content"])
    return f"wrote {len(args['content'])} chars to {_rel(p)}"


@tool(
    "list_dir",
    "List files in a workspace directory (recursive; size + type per entry).",
    {"path": "string (optional, default '.')"},
    permission="allow",
)
def list_dir(args: dict) -> str:
    p = safe_path(args.get("path") or ".")
    if not p.is_dir():
        return f"ERROR: not a directory: {_rel(p)}"
    lines = []
    for e in sorted(p.rglob("*"), key=lambda x: (str(x).lower())):
        try:
            if e.is_file():
                lines.append(f"{_rel(e)}  {e.stat().st_size:>8} B")
            else:
                lines.append(f"{_rel(e)}/  <dir>")
        except OSError:
            continue
    if not lines:
        return f"(empty: {_rel(p)})"
    return "\n".join(lines)
