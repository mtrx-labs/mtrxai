"""In-process .gguf backend via llama-cpp-python. Lazy: costs nothing until first use."""

from __future__ import annotations

import asyncio
import gc
from pathlib import Path
from typing import Any

from .base import ModelError, Msg


class GGUFBackend:
    def __init__(self, model_path: str, n_ctx: int = 8192, n_gpu_layers: int = -1):
        if not Path(model_path).is_file():
            raise ModelError(
                f".gguf file not found: {model_path} — fix the model ref or the path"
            )
        self.model_path = model_path
        self.n_ctx = n_ctx
        self.n_gpu_layers = n_gpu_layers
        self._llm: Any = None

    def _load(self) -> None:
        try:
            from llama_cpp import Llama
        except ImportError as e:
            raise ModelError(
                "llama-cpp-python is not installed — run: pip install -e '.[gguf]'"
            ) from e
        self._llm = Llama(
            model_path=self.model_path,
            n_ctx=self.n_ctx,
            n_gpu_layers=self.n_gpu_layers,
            verbose=False,
        )

    async def chat(
        self, messages: list[Msg], *, temperature: float = 0.4, json_mode: bool = False, **_: Any
    ) -> str:
        if self._llm is None:
            await asyncio.to_thread(self._load)
        kwargs: dict[str, Any] = {"messages": messages, "temperature": temperature}
        if json_mode:
            kwargs["response_format"] = {"type": "json_object"}
        try:
            out = await asyncio.to_thread(self._llm.create_chat_completion, **kwargs)
        except TypeError:
            # older llama-cpp builds without response_format support
            kwargs.pop("response_format", None)
            out = await asyncio.to_thread(self._llm.create_chat_completion, **kwargs)
        return out["choices"][0]["message"]["content"]

    def unload(self) -> None:
        if self._llm is not None:
            try:
                self._llm.close()
            except Exception:
                pass
            self._llm = None
            gc.collect()  # release RAM/VRAM promptly (verify: free -h / nvidia-smi)
