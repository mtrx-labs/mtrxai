""""hey matrix" — openWakeWord wrapper for the custom mtrxAI.onnx model."""

from __future__ import annotations

import time
from pathlib import Path

import numpy as np

from .errors import VoiceError


class WakeWord:
    def __init__(self, model_path, threshold: float = 0.5, refractory_s: float = 2.0):
        path = Path(model_path).expanduser()
        if not path.is_file():
            raise VoiceError(
                f"wake model not found: {path} — copy your .onnx to assets/wake/ "
                f"or run: python -m mtrxai.voice.setup"
            )
        try:
            from openwakeword.model import Model
        except ImportError as e:
            raise VoiceError("openwakeword is not installed — pip install -e '.[voice]'") from e
        try:
            self.m = Model(wakeword_models=[str(path)], inference_framework="onnx")
        except Exception as e:
            raise VoiceError(
                f"wake model failed to load ({e}) — run: python -m mtrxai.voice.setup"
            ) from e
        self.name = path.stem
        self.threshold = threshold
        self.refractory_s = refractory_s
        self._cool_until = 0.0
        self.last_score = 0.0

    def score(self, frame: np.ndarray) -> float:
        """Raw prediction score for one frame (calibration)."""
        scores = self.m.predict(frame) or {}
        self.last_score = max(scores.values()) if scores else 0.0
        return self.last_score

    def hit(self, frame: np.ndarray, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        if now < self._cool_until:
            return False
        if self.score(frame) >= self.threshold:
            self.m.reset()
            self._cool_until = now + self.refractory_s
            return True
        return False
