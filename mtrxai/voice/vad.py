"""End-of-utterance detection: webrtcvad over 20 ms sub-frames of the 80 ms blocks."""

from __future__ import annotations

import numpy as np

from .errors import VoiceError

SAMPLE_RATE = 16_000
SUBFRAME = 320  # samples → 20 ms of int16


class VadEndDetector:
    """feed() each 80 ms block; True means 'utterance complete'."""

    def __init__(self, silence_ms: int = 700, max_utterance_s: float = 15.0, mode: int = 2):
        try:
            import webrtcvad
        except ImportError as e:
            raise VoiceError("webrtcvad is not installed — pip install -e '.[voice]'") from e
        self.vad = webrtcvad.Vad(mode)
        self.silence_ms = int(silence_ms)
        self.max_ms = int(max_utterance_s * 1000)

    def reset(self) -> None:
        self._silence_ms = 0
        self._total_ms = 0
        self._heard_speech = False

    def feed(self, frame: np.ndarray) -> bool:
        for sub in frame.reshape(-1, SUBFRAME):
            speech = self.vad.is_speech(sub.astype(np.int16).tobytes(), SAMPLE_RATE)
            self._total_ms += 20
            if speech:
                self._heard_speech = True
                self._silence_ms = 0
            else:
                self._silence_ms += 20
            if self._heard_speech and self._silence_ms >= self.silence_ms:
                return True
            if self._total_ms >= self.max_ms:
                return True
        return False

    # set in reset(); kept here so attribute exists before first reset()
    _silence_ms = 0
    _total_ms = 0
    _heard_speech = False
