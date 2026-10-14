"""OpenAI-compatible chat backends: OpenAI, OpenRouter, Groq, llama-server, LM Studio,
vLLM… plus Anthropic's Messages API (different wire shape)."""

from __future__ import annotations

import os
from typing import Any

import httpx

from .base import ModelError, Msg, raise_for_model


class OpenAICompatBackend:
    def __init__(self, model: str, base_url: str, api_key_env: str = "", label: str = "API"):
        self.model = model
        self.base_url = base_url.rstrip("/")
        self.api_key_env = api_key_env
        self.label = label
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(300, connect=10))

    def _headers(self) -> dict[str, str]:
        token = os.environ.get(self.api_key_env, "") if self.api_key_env else ""
        return {"Authorization": f"Bearer {token}"} if token else {}

    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, json_mode: bool = False, **_: Any
    ) -> str:
        if self.api_key_env and not os.environ.get(self.api_key_env, ""):
            raise ModelError(
                f"{self.api_key_env} is not set — add it to .env "
                f"(provider '{self.label}')"
            )
        body: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature,
        }
        if json_mode:
            body["response_format"] = {"type": "json_object"}
        url = f"{self.base_url}/chat/completions"
        try:
            r = await self._client.post(url, json=body, headers=self._headers())
            if r.status_code == 400 and json_mode and "response_format" in r.text:
                # some compat servers reject response_format — retry without it
                body.pop("response_format", None)
                r = await self._client.post(url, json=body, headers=self._headers())
        except httpx.ConnectError as e:
            raise ModelError(
                f"{self.label} is not reachable at {self.base_url} ({e})"
            ) from e
        except httpx.HTTPError as e:
            raise ModelError(f"{self.label} request failed: {e}") from e
        raise_for_model(self.label, r)
        try:
            return r.json()["choices"][0]["message"]["content"]
        except (KeyError, IndexError, ValueError) as e:
            raise ModelError(f"{self.label} returned an unexpected response: {r.text[:200]}") from e

    def unload(self) -> None:
        pass


class AnthropicBackend:
    """Anthropic Messages API: system prompt is top-level, auth via x-api-key."""

    def __init__(self, model: str, api_key_env: str = "ANTHROPIC_API_KEY"):
        self.model = model
        self.api_key_env = api_key_env
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(300, connect=10))
        self._url = "https://api.anthropic.com/v1/messages"

    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, max_tokens: int = 4096, **_: Any
    ) -> str:
        key = os.environ.get(self.api_key_env, "")
        if not key:
            raise ModelError(f"{self.api_key_env} is not set — add it to .env")
        system = "\n".join(m["content"] for m in messages if m["role"] == "system")
        rest = [m for m in messages if m["role"] != "system"]
        body = {
            "model": self.model,
            "system": system,
            "messages": rest,
            "max_tokens": max_tokens,
            "temperature": temperature,
        }
        headers = {"x-api-key": key, "anthropic-version": "2023-06-01"}
        try:
            r = await self._client.post(self._url, json=body, headers=headers)
        except httpx.HTTPError as e:
            raise ModelError(f"Anthropic request failed: {e}") from e
        raise_for_model("Anthropic", r)
        try:
            return "".join(
                b["text"] for b in r.json()["content"] if b.get("type") == "text"
            )
        except (KeyError, ValueError) as e:
            raise ModelError(f"Anthropic returned an unexpected response: {r.text[:200]}") from e

    def unload(self) -> None:
        pass
