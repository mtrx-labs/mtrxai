"""Web tools: search (ddgs) and page fetch. Both run in worker threads."""

from __future__ import annotations

import html as html_mod
import re

import httpx

from .base import tool

MAX_FETCH_CHARS = 12_000

_SCRIPT_STYLE = re.compile(r"<(script|style)[^>]*>.*?</\1>", re.I | re.S)
_TAGS = re.compile(r"<[^>]+>")
_WS = re.compile(r"\n{3,}")


def _html_to_text(body: str) -> str:
    body = _SCRIPT_STYLE.sub(" ", body)
    body = _TAGS.sub(" ", body)
    body = html_mod.unescape(body)
    lines = [ln.strip() for ln in body.splitlines()]
    return _WS.sub("\n\n", "\n".join(lines)).strip()


@tool(
    "web_search",
    "Search the web; returns title, url and snippet per result.",
    {"query": "string"},
    permission="allow",
)
def web_search(args: dict) -> str:
    try:
        from ddgs import DDGS
    except ImportError:
        return "ERROR: ddgs is not installed — run: pip install -e '.[web]'"
    try:
        results = DDGS().text(args["query"], max_results=5)
    except Exception as e:  # noqa: BLE001 — network errors are model-visible
        return f"ERROR: search failed: {type(e).__name__}: {e}"
    if not results:
        return f"no results for: {args['query']}"
    return "\n".join(
        f"- {r.get('title', '')} ({r.get('href', '')}): {r.get('body', '')[:250]}"
        for r in results
    )


@tool(
    "web_fetch",
    "Fetch a web page and return its readable text (HTML tags stripped).",
    {"url": "string"},
    permission="allow",
)
def web_fetch(args: dict) -> str:
    try:
        with httpx.Client(
            timeout=30, follow_redirects=True, headers={"User-Agent": "mtrxai/0.2"}
        ) as c:
            r = c.get(args["url"])
        r.raise_for_status()
    except httpx.HTTPError as e:
        return f"ERROR: fetch failed: {type(e).__name__}: {e}"
    ctype = r.headers.get("content-type", "")
    if "html" in ctype or "xml" in ctype or "text/" in ctype or "json" in ctype:
        text = _html_to_text(r.text) if "html" in ctype else r.text
    else:
        return f"unsupported content-type: {ctype} ({len(r.content)} bytes)"
    if len(text) > MAX_FETCH_CHARS:
        text = text[:MAX_FETCH_CHARS] + "\n… truncated"
    return text
