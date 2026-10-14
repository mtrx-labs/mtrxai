"""Voice state-machine tests — fully headless: no mic, no models, no audio libs."""

import threading
import time
from contextlib import contextmanager

import numpy as np
import pytest

from fake import FakeSTT, FakeVAD, FakeWake, silent_frame

from mtrxai.voice.pipeline import VoiceEngine, VoiceService

VOICE_CFG = {
    "wake_model": "assets/wake/mtrxAI.onnx",
    "wake_threshold": 0.5,
    "refractory_s": 2.0,
    "silence_ms": 700,
    "max_utterance_s": 15.0,
    "prebuffer_ms": 300,
    "stt": "faster-whisper:base.en",
}


# ------------------------------------------------------------------- the engine


def test_engine_wake_then_recording_then_utterance():
    wake, vad = FakeWake(trigger_at=3), FakeVAD(end_after=4)
    eng = VoiceEngine(wake, vad, prebuffer_ms=240)  # 3 prebuffer frames

    assert eng.feed(silent_frame(), 1.0) == ("none", None)
    assert eng.feed(silent_frame(), 2.0) == ("none", None)
    assert eng.state == "armed"
    assert eng.feed(silent_frame(), 3.0) == ("wake", None)  # FakeWake triggers on frame 3
    assert eng.state == "recording"

    for i in range(3):
        assert eng.feed(silent_frame(), 4.0 + i) == ("none", None)
    kind, payload = eng.feed(silent_frame(), 8.0)  # 4th recording frame ends it
    assert kind == "utterance"
    assert isinstance(payload, np.ndarray)
    # 2 prebuffered + 1 trigger + 4 recording frames, no duplicates
    assert len(payload) == 7 * 1280
    assert eng.state == "armed"


def test_engine_refractory_is_honored_by_wake_wrapper():
    """WakeWord-level refractory is covered by real wake tests; engine stays dumb."""
    wake, vad = FakeWake(trigger_at=1), FakeVAD(end_after=1)
    eng = VoiceEngine(wake, vad)
    assert eng.feed(silent_frame(), 1.0)[0] == "wake"
    assert eng.feed(silent_frame(), 1.1)[0] == "utterance"  # ends immediately


# ----------------------------------------------------------------- the service


class FakeEngineService(VoiceService):
    """Real VoiceEngine with fakes, real queue loop, no audio hardware."""

    def _build_engine(self):
        return VoiceEngine(FakeWake(trigger_at=3), FakeVAD(end_after=4),
                           prebuffer_ms=240)

    @contextmanager
    def _open_stream(self):
        svc = self

        def feed():
            for _ in range(20):
                if svc.stop_event.is_set():
                    return
                svc.q.put(silent_frame())
                time.sleep(0.001)
            svc.stop_event.set()  # no more audio → end the service

        threading.Thread(target=feed, daemon=True).start()
        yield


def test_voice_service_end_to_end_with_fakes():
    got_text = threading.Event()
    statuses: list[str] = []
    heard: list[str] = []

    svc = FakeEngineService(dict(VOICE_CFG), {
        "status": lambda s: statuses.append(s),
        "text": lambda t: (heard.append(t), got_text.set()),
    })
    svc._stt = FakeSTT("run the workspace tests")
    svc.start()
    assert got_text.wait(timeout=10), f"no transcription; statuses={statuses}"
    svc.stop()

    assert heard == ["run the workspace tests"]
    assert svc.error is None
    assert "armed" in statuses
    assert any("recording" in s for s in statuses)
    assert any("transcribing" in s for s in statuses)
    assert len(svc._stt.fed) == 1 and len(svc._stt.fed[0]) == 7 * 1280


def test_engine_build_failure_reports_error_status():
    class FailingService(VoiceService):
        def _build_engine(self):
            raise RuntimeError("openwakeword is not installed")

        def _open_stream(self):  # pragma: no cover — never reached
            @contextmanager
            def cm():
                yield
            return cm()

    statuses: list[str] = []
    svc = FailingService(dict(VOICE_CFG), {"status": lambda s: statuses.append(s),
                                           "text": lambda t: None})
    svc.start()
    svc.join(timeout=5)
    assert svc.error is not None
    assert any("error" in s for s in statuses)


def test_vad_cap_when_never_speech():
    """Real webrtcvad (if installed): pure silence never 'ends' until the time cap."""
    pytest.importorskip("webrtcvad")
    from mtrxai.voice.vad import VadEndDetector

    vad = VadEndDetector(silence_ms=700, max_utterance_s=1.0)  # cap at 1000 ms
    ended_at = None
    for i in range(20):  # 80 ms each → 1.6 s of silence
        if vad.feed(silent_frame()):
            ended_at = (i + 1) * 80
            break
    assert ended_at == 1000
