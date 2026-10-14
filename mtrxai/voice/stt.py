"""Swappable speech-to-text backends: faster-whisper (local) or OpenAI Whisper (cloud)."""

from __future__ import annotations

import io
import os
import wave

import httpx
import numpy as np

from .errors import VoiceError


def pcm_to_wav_bytes(pcm: np.ndarray, sr: int = 16_000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.astype(np.int16).tobytes())
    return buf.getvalue()


class FasterWhisperSTT:
    """Local CT2 whisper — int8 on CPU is fine for short utterances."""

    def __init__(self, model: str = "base.en"):
        try:
            from faster_whisper import WhisperModel
        except ImportError as e:
            raise VoiceError(
                "faster-whisper is not installed — pip install -e '.[voice]'"
            ) from e
        self.m = WhisperModel(model, device="cpu", compute_type="int8")

    def transcribe(self, pcm: np.ndarray) -> str:
        audio = pcm.astype(np.float32) / 32768.0
        segments, _ = self.m.transcribe(audio, beam_size=1, vad_filter=True)
        return " ".join(s.text.strip() for s in segments).strip()


class OpenAIWhisperSTT:
    def __init__(self, model: str = "whisper-1"):
        self.model = model
        self.key = os.environ.get("OPENAI_API_KEY", "")

    def transcribe(self, pcm: np.ndarray) -> str:
        if not self.key:
            raise VoiceError("OPENAI_API_KEY is not set — add it to .env")
        try:
            r = httpx.post(
                "https://api.openai.com/v1/audio/transcriptions",
                headers={"Authorization": f"Bearer {self.key}"},
                files={"file": ("utterance.wav", pcm_to_wav_bytes(pcm), "audio/wav")},
                data={"model": self.model},
                timeout=120,
            )
            r.raise_for_status()
            return r.json().get("text", "").strip()
        except httpx.HTTPError as e:
            raise VoiceError(f"OpenAI STT failed: {e}") from e


def make_stt(ref: str):
    """'provider:model' → STT backend, mirroring the model-layer factory."""
    provider, sep, model = ref.partition(":")
    if not sep:
        provider, model = "faster-whisper", ref
    if provider in ("faster-whisper", "fasterwhisper", "local"):
        return FasterWhisperSTT(model or "base.en")
    if provider == "openai":
        return OpenAIWhisperSTT(model or "whisper-1")
    raise VoiceError(
        f"unknown stt provider '{provider}' — use faster-whisper:<model> or openai:whisper-1"
    )
