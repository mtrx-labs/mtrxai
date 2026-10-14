"""Settings & environment loading. Paths resolve relative to the project root."""

from __future__ import annotations

import os
import tomllib
from pathlib import Path
from typing import Any

DEFAULTS: dict[str, Any] = {
    "workspace": "./workspace",
    "logs": "./logs",
    "agents_dir": "./agents",
    "max_steps": 8,
    "default_permission": "ask",
    "auto_create_agents": False,
    "router_agent": "router",
    "default_agent_model": "ollama:llama3.1:8b",
    "providers": {
        "ollama": {"base_url": "http://127.0.0.1:11434"},
        "openai": {"base_url": "https://api.openai.com/v1", "api_key_env": "OPENAI_API_KEY"},
        "anthropic": {"api_key_env": "ANTHROPIC_API_KEY"},
        "llamacpp": {"base_url": "http://127.0.0.1:8080/v1", "api_key_env": ""},
    },
    "gguf": {"n_ctx": 8192, "n_gpu_layers": -1},
    "voice": {},
}

VOICE_DEFAULTS: dict[str, Any] = {
    "enabled": False,
    "wake_model": "assets/wake/mtrxAI.onnx",
    "wake_threshold": 0.5,
    "refractory_s": 2.0,
    "silence_ms": 700,
    "max_utterance_s": 15.0,
    "prebuffer_ms": 300,
    "stt": "faster-whisper:base.en",
    "tts": "piper:en_US-amy-medium",
    "voices_dir": "~/.local/share/piper",
    "auto_submit": False,
    "speak_responses": True,
    "input_device": "",
}


def find_project_root(cli_path: str | None = None) -> Path:
    """Where settings.toml / agents/ / workspace/ live, in priority order."""
    candidates: list[Path] = []
    if cli_path:
        candidates.append(Path(cli_path).expanduser().resolve())
    if os.environ.get("MTRXAI_PROJECT"):
        candidates.append(Path(os.environ["MTRXAI_PROJECT"]).expanduser().resolve())
    candidates.append(Path.cwd())
    candidates.append(Path(__file__).resolve().parent.parent)  # editable-install repo root
    for c in candidates:
        if (c / "settings.toml").is_file():
            return c
    return candidates[0] if cli_path else candidates[-1]


def load_env(root: Path) -> None:
    """Load .env into the environment without clobbering existing variables."""
    env_file = root / ".env"
    if not env_file.is_file():
        return
    for line in env_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip().strip('"').strip("'")
        if key:
            os.environ.setdefault(key, value)


def _resolve(root: Path, value: str) -> Path:
    p = Path(value).expanduser()
    return p if p.is_absolute() else (root / p).resolve()


def load_settings(root: Path) -> dict:
    merged = {k: (dict(v) if isinstance(v, dict) else v) for k, v in DEFAULTS.items()}
    file = root / "settings.toml"
    if file.is_file():
        with file.open("rb") as fh:
            data = tomllib.load(fh)
        for key, value in data.items():
            if isinstance(value, dict) and isinstance(merged.get(key), dict):
                merged[key].update(value)
            else:
                merged[key] = value

    merged["project_root"] = root
    merged["workspace"] = _resolve(root, merged["workspace"])
    merged["logs"] = _resolve(root, merged["logs"])
    merged["agents_dir"] = _resolve(root, merged["agents_dir"])
    merged["workspace"].mkdir(parents=True, exist_ok=True)
    merged["logs"].mkdir(parents=True, exist_ok=True)

    providers = merged.setdefault("providers", {})
    providers.setdefault("ollama", {"base_url": "http://127.0.0.1:11434"})

    voice = dict(VOICE_DEFAULTS)
    voice.update(merged.get("voice") or {})
    wake = _resolve(root, str(voice["wake_model"]))
    if not wake.is_file():  # fall back to the user's home-dir copy
        home_copy = Path.home() / "mtrxAI.onnx"
        if home_copy.is_file():
            wake = home_copy
    voice["wake_model"] = wake
    voice["voices_dir"] = _resolve(root, str(voice["voices_dir"]))
    voice["silence_ms"] = int(voice["silence_ms"])
    voice["prebuffer_ms"] = int(voice["prebuffer_ms"])
    voice["max_utterance_s"] = float(voice["max_utterance_s"])
    voice["refractory_s"] = float(voice["refractory_s"])
    voice["wake_threshold"] = float(voice["wake_threshold"])
    merged["voice"] = voice
    return merged
