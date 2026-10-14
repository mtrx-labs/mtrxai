"""Ollama HTTP API backend (default local provider)."""

from __future__ import annotations

from typing import Any

import httpx

from .base import ModelError, Msg, raise_for_model


class OllamaBackend:
    def __init__(self, model: str, base_url: str = "http://127.0.0.1:11434"):
        self.model = model
        self.base_url = base_url.rstrip("/")
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(300, connect=5))

    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, json_mode: bool = False, **_: Any
    ) -> str:
        payload: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "stream": False,
            "options": {"temperature": temperature},
        }
        if json_mode:
            payload["format"] = "json"  # constrained decoding → reliable routing
        try:
            r = await self._client.post(f"{self.base_url}/api/chat", json=payload)
        except httpx.ConnectError as e:
            raise ModelError(
                f"Ollama is not reachable at {self.base_url} — start it with "
                f"`systemctl start ollama` (or `ollama serve`). ({e})"
            ) from e
        except httpx.HTTPError as e:
            raise ModelError(f"Ollama request failed: {e}") from e
        if r.status_code == 404:
            raise ModelError(
                f"model '{self.model}' is not in Ollama — run: ollama pull {self.model}"
            )
        raise_for_model("Ollama", r)
        return r.json().get("message", {}).get("content", "")

    def unload(self) -> None:
        pass  # server-based backends are stateless

    @staticmethod
    async def list_models(base_url: str = "http://127.0.0.1:11434") -> list[str]:
        """Names of locally available models; empty list if the server is down."""
        try:
            async with httpx.AsyncClient(timeout=5) as c:
                r = await c.get(f"{base_url.rstrip('/')}/api/tags")
                r.raise_for_status()
                return [m["name"] for m in r.json().get("models", [])]
        except Exception:
            return []
