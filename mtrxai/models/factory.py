"""'provider:model' reference → backend instance. This is where hot-swap is born."""

from __future__ import annotations

from .base import ModelBackend, ModelError
from .fake import FakeBackend
from .gguf import GGUFBackend
from .ollama import OllamaBackend
from .openai_compat import AnthropicBackend, OpenAICompatBackend

BUILTIN_PROVIDERS = ("ollama", "gguf", "anthropic", "fake")


def make_backend(ref: str, providers: dict, gguf_cfg: dict | None = None) -> ModelBackend:
    ref = ref.strip()
    provider, sep, model = ref.partition(":")
    if not sep or not model:
        raise ModelError(f"bad model reference '{ref}' — expected provider:model")
    if provider == "ollama":
        base = (providers.get("ollama") or {}).get("base_url", "http://127.0.0.1:11434")
        return OllamaBackend(model, base)
    if provider == "gguf":
        cfg = gguf_cfg or {}
        return GGUFBackend(
            model, n_ctx=int(cfg.get("n_ctx", 8192)), n_gpu_layers=int(cfg.get("n_gpu_layers", -1))
        )
    if provider == "anthropic":
        key_env = (providers.get("anthropic") or {}).get("api_key_env", "ANTHROPIC_API_KEY")
        return AnthropicBackend(model, key_env)
    if provider == "fake":
        return FakeBackend(model)
    cfg = providers.get(provider)
    if not isinstance(cfg, dict) or "base_url" not in cfg:
        known = ", ".join((*BUILTIN_PROVIDERS, *providers))
        raise ModelError(f"unknown provider '{provider}' — known providers: {known}")
    return OpenAICompatBackend(
        model, cfg["base_url"], cfg.get("api_key_env", ""), label=provider
    )
