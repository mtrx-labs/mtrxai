"""JSONL run trace: every routing decision, tool call and answer, auditable."""

from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path


class Trace:
    def __init__(self, logs_dir: Path):
        logs_dir.mkdir(parents=True, exist_ok=True)
        self.path = logs_dir / "trace.jsonl"
        self.last: list[dict] = []
        self.last_task: str = ""

    def log(self, type_: str, **kw) -> None:
        entry = {"ts": datetime.now().isoformat(timespec="seconds"), "type": type_, **kw}
        with self.path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False, default=str) + "\n")
        if type_ == "task":  # a new task starts a new in-memory "last run"
            self.last = []
            self.last_task = kw.get("task", "")
        self.last.append(entry)
        if len(self.last) > 400:
            self.last = self.last[-400:]

    def format_last(self) -> str:
        if not self.last:
            return "(no run yet — send a task first)"
        lines = [f"task: {self.last_task}"]
        for e in self.last:
            t = e["type"]
            if t == "task":
                continue
            if t == "route":
                lines.append(
                    f"  route → {e.get('chosen')} (raw: {str(e.get('raw', ''))[:120]})"
                )
            elif t == "tool_call":
                lines.append(f"  {e.get('agent')} · {e.get('tool')} {json.dumps(e.get('args', {}))[:200]}")
            elif t == "tool_result":
                lines.append(f"    ↳ {str(e.get('result', ''))[:160]}")
            elif t == "answer":
                lines.append(f"  answer ({e.get('agent')}): {str(e.get('text', ''))[:300]}")
            else:
                rest = {k: v for k, v in e.items() if k not in ("ts", "type")}
                lines.append(f"  {t}: {json.dumps(rest, default=str)[:200]}")
        return "\n".join(lines)
