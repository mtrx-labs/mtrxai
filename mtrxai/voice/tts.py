"""Swappable text-to-speech backends: Piper (local .onnx voice) or OpenAI TTS (cloud)."""

from __future__ import annotations

import io
import os
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path

import httpx
import numpy as np

from .audio import play_int16
from .errors import VoiceError


def _wav_bytes_to_pcm(data: bytes) -> tuple[np.ndarray, int]:
    with wave.open(io.BytesIO(data), "rb") as w:
        sr = w.getframerate()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return pcm, sr


class PiperTTS:
    """Speaks via the `piper` CLI (piper-tts) and plays the resulting wav."""

    def __init__(self, voice: str, voices_dir):
        self.voice = voice
        self.dir = Path(voices_dir).expanduser()
        self.model_path = self.dir / f"{voice}.onnx"
        self.config_path = self.dir / f"{voice}.onnx.json"
        self.binary = shutil.which("piper")

    @property
    def available(self) -> bool:
        return bool(self.binary) and self.model_path.is_file() and self.config_path.is_file()

    def _hint(self) -> str:
        return (
            f"piper voice '{self.voice}' is not set up — run: "
            f"python -m piper.download_voices {self.voice} "
            f"(or python -m mtrxai.voice.setup)"
            if self.binary
            else "piper CLI not found — pip install -e '.[voice]'"
        )

    def speak(self, text: str, stop=None) -> None:
        if not self.available:
            raise VoiceError(self._hint())
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
            out = Path(tf.name)
        try:
            subprocess.run(
                [self.binary, "-m", str(self.model_path), "-f", str(out)],
                input=text.encode(),
                check=True,
                timeout=120,
                capture_output=True,
            )
            pcm, sr = _wav_bytes_to_pcm(out.read_bytes())
        finally:
            out.unlink(missing_ok=True)
        play_int16(pcm, sr, stop)


class OpenAITTS:
    def __init__(self, model: str = "tts-1", voice: str = "alloy"):
        self.model = model
        self.voice = voice

    def speak(self, text: str, stop=None) -> None:
        key = os.environ.get("OPENAI_API_KEY", "")
        if not key:
            raise VoiceError("OPENAI_API_KEY is not set — add it to .env")
        try:
            r = httpx.post(
                "https://api.openai.com/v1/audio/speech",
                headers={"Authorization": f"Bearer {key}"},
                json={
                    "model": self.model,
                    "input": text[:4000],
                    "voice": self.voice,
                    "response_format": "wav",
                },
                timeout=120,
            )
            r.raise_for_status()
            pcm, sr = _wav_bytes_to_pcm(r.content)
        except httpx.HTTPError as e:
            raise VoiceError(f"OpenAI TTS failed: {e}") from e
        play_int16(pcm, sr, stop)


def make_tts(ref: str, voices_dir=None):
    """'provider:model' → TTS backend."""
    provider, sep, model = ref.partition(":")
    if not sep:
        provider, model = "piper", ref
    if provider == "piper":
        return PiperTTS(model or "en_US-amy-medium", voices_dir or "~/.local/share/piper")
    if provider == "openai":
        return OpenAITTS(model or "tts-1")
    raise VoiceError(
        f"unknown tts provider '{provider}' — use piper:<voice> or openai:tts-1"
    )
