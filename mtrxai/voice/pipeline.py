"""The voice service thread: ARMED → RECORDING → TRANSCRIBING → back to ARMED.

Design split:
  - VoiceEngine is pure frame logic (unit-testable with fakes, no audio hardware)
  - VoiceService owns the thread, the audio stream, and the lazy STT
"""

from __future__ import annotations

import queue
import threading
import time
from collections import deque
from typing import Any, Callable

import numpy as np

from .errors import VoiceError

Frame = np.ndarray  # int16 PCM, 80 ms at 16 kHz (1280 samples)
Hooks = dict[str, Callable[[str], None]]


class VoiceEngine:
    """Frame-level state machine. feed() returns what happened, service reacts."""

    def __init__(self, wake, vad, prebuffer_ms: int = 300, block_ms: int = 80):
        self.wake = wake
        self.vad = vad
        self.state = "armed"
        self._pre = deque(maxlen=max(1, prebuffer_ms // block_ms))
        self._frames: list[Frame] = []

    def feed(self, frame: Frame, now: float) -> tuple[str, Any]:
        if self.state == "armed":
            if self.wake.hit(frame, now):
                self.state = "recording"
                self._frames = [f.copy() for f in self._pre] + [frame.copy()]
                self._pre.clear()
                return ("wake", None)
            self._pre.append(frame.copy())
            return ("none", None)

        # recording
        self._frames.append(frame.copy())
        if self.vad.feed(frame):
            utterance = np.concatenate(self._frames)
            self._frames = []
            self.vad.reset()
            self.state = "armed"
            return ("utterance", utterance)
        return ("none", None)


class VoiceService(threading.Thread):
    """Runs the whole loop off the UI thread; hands transcribed text to `hooks`."""

    def __init__(self, cfg: dict, hooks: Hooks):
        super().__init__(daemon=True, name="mtrxai-voice")
        self.cfg = cfg
        self.hooks = hooks
        self.q: "queue.Queue[Frame]" = queue.Queue()
        self.stop_event = threading.Event()
        self.error: str | None = None
        self.engine: VoiceEngine | None = None
        self._stt = None

    # -------------------------------------------------- overridable seams

    def _build_engine(self) -> VoiceEngine:
        from .vad import VadEndDetector
        from .wake import WakeWord

        wake = WakeWord(
            self.cfg["wake_model"],
            threshold=float(self.cfg.get("wake_threshold", 0.5)),
            refractory_s=float(self.cfg.get("refractory_s", 2.0)),
        )
        vad = VadEndDetector(
            silence_ms=int(self.cfg.get("silence_ms", 700)),
            max_utterance_s=float(self.cfg.get("max_utterance_s", 15.0)),
        )
        return VoiceEngine(wake, vad, prebuffer_ms=int(self.cfg.get("prebuffer_ms", 300)))

    def _open_stream(self):
        from .audio import input_stream

        return input_stream(self._cb, device=self.cfg.get("input_device") or None)

    def _get_stt(self):
        if self._stt is None:
            from .stt import make_stt

            self._stt = make_stt(self.cfg.get("stt", "faster-whisper:base.en"))
        return self._stt

    # -------------------------------------------------- thread body

    def _cb(self, indata, frames, time_info, status) -> None:
        self.q.put(indata.copy())  # the callback does nothing else

    def _status(self, text: str) -> None:
        try:
            self.hooks["status"](text)
        except Exception:
            pass  # UI may be shutting down

    def run(self) -> None:
        try:
            self.engine = self._build_engine()
        except (VoiceError, Exception) as e:  # noqa: BLE001
            self.error = str(e)
            self._status(f"voice error: {e}")
            return
        try:
            with self._open_stream():
                self._status("armed")
                while not self.stop_event.is_set():
                    try:
                        frame = self.q.get(timeout=0.5)
                    except queue.Empty:
                        continue
                    kind, payload = self.engine.feed(frame, time.monotonic())
                    if kind == "wake":
                        self._status("recording…")
                    elif kind == "utterance":
                        self._status("transcribing…")
                        try:
                            text = self._get_stt().transcribe(payload)
                        except (VoiceError, Exception) as e:  # noqa: BLE001
                            self.error = str(e)
                            self._status(f"stt error: {e}")
                            continue
                        self._status("armed")
                        if text:
                            try:
                                self.hooks["text"](text)
                            except Exception:
                                pass
        except (VoiceError, Exception) as e:  # noqa: BLE001
            self.error = str(e)
            self._status(f"voice error: {e}")
            return
        self._status("off")

    def stop(self) -> None:
        self.stop_event.set()
        self.join(timeout=3)
