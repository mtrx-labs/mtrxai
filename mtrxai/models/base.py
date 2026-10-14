"""The one interface every model backend reduces to."""

from __future__ import annotations

from typing import Any, Protocol, runtime_checkable

import httpx

# {"role": "system"|"user"|"assistant", "content": "..."}
Msg = dict[str, str]


class ModelError(RuntimeError):
    """A backend cannot answer. The message is user-facing — show it directly."""


@runtime_checkable
class ModelBackend(Protocol):
    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, json_mode: bool = False, **opts: Any
    ) -> str: ...

    def unload(self) -> None: ...


def raise_for_model(prefix: str, resp: httpx.Response) -> None:
    """Turn a non-2xx HTTP response into a ModelError with an actionable hint."""
    if resp.is_success:
        return
    body = resp.text[:300].replace("\n", " ")
    hint = {
        401: " — check the API key in your .env",
        403: " — key present but not authorized for this model",
        404: " — model or endpoint not found",
        429: " — rate limited / quota exceeded",
    }.get(resp.status_code, "")
    raise ModelError(f"{prefix} returned {resp.status_code}{hint}: {body}")
