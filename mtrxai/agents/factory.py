"""Proposal → validated AgentSpec. Agents are data; creation is a YAML write."""

from __future__ import annotations

import re

from ..tools.base import TOOLBOX
from .agent import Agent  # noqa: F401 — re-exported for convenience
from ..core.registry import AgentSpec, Registry

_KEBAB = re.compile(r"^[a-z0-9][a-z0-9-]*$")
_STOPWORDS = {
    "the", "a", "an", "and", "or", "for", "with", "from", "that", "this", "into",
    "your", "you", "its", "are", "was", "will", "shall", "can", "may", "new",
}


def slugify(name: str) -> str:
    s = re.sub(r"[^a-z0-9-]+", "-", name.strip().lower()).strip("-")
    return s[:48]


def _keywords_from(description: str) -> list[str]:
    words = [
        w.strip(".,;:!?")
        for w in description.lower().split()
        if len(w) > 3 and w.strip(".,;:!?") not in _STOPWORDS
    ]
    seen: list[str] = []
    for w in words:
        if w not in seen:
            seen.append(w)
    return seen[:6]


def create_agent_spec(proposal: dict, default_model: str) -> AgentSpec:
    """Validate and normalize a (possibly LLM-drafted) proposal into an AgentSpec."""
    name = slugify(str(proposal.get("name", "")))
    if not name or not _KEBAB.match(name):
        raise ValueError(
            f"bad agent name '{proposal.get('name')!r}' — use kebab-case, e.g. 'data-analyst'"
        )
    description = str(proposal.get("description") or "").strip() or f"Specialist: {name}."
    if len(description) > 200:
        description = description[:197] + "…"

    raw_tools = proposal.get("tools") or []
    if isinstance(raw_tools, str):
        raw_tools = [t.strip() for t in raw_tools.split(",") if t.strip()]
    unknown = [t for t in raw_tools if t not in TOOLBOX]
    if unknown:
        raise ValueError(
            f"unknown tool(s) {unknown} — available: {', '.join(sorted(TOOLBOX))}"
        )
    tools = list(dict.fromkeys(raw_tools))

    model = str(proposal.get("model") or default_model).strip()
    temperature = float(proposal.get("temperature", 0.4))

    keywords = proposal.get("keywords")
    if not isinstance(keywords, list) or not keywords:
        keywords = _keywords_from(f"{name} {description}")

    title = name.replace("-", " ").capitalize()
    system_prompt = str(proposal.get("system_prompt") or "").strip() or (
        f"You are {title}, {description} Work step by step, verify your work, "
        f"and be concise."
    )

    return AgentSpec(
        name=name,
        description=description,
        model=model,
        system_prompt=system_prompt,
        tools=tools,
        temperature=temperature,
        permissions={t: "ask" for t in tools if t == "run_shell"},
        keywords=[str(k) for k in keywords][:12],
    )


def write_agent_yaml(spec: AgentSpec, registry: Registry):
    """Persist a new agent and reload the registry (with approval already given)."""
    return registry.save_spec(spec)
