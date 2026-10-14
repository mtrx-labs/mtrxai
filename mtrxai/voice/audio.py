"""Audio capture/playback helpers around sounddevice (PortAudio → PipeWire)."""

from __future__ import annotations

from contextlib import contextmanager

import numpy as np

from .errors import VoiceError

SAMPLE_RATE = 16_000
BLOCK_MS = 80  # 4 × 20 ms VAD frames
BLOCK_SIZE = SAMPLE_RATE * BLOCK_MS // 1000  # 1280 samples


class _Resampler:
    """Linear-interpolation resampler with carry, in→16 kHz, 80 ms chunks out."""

    def __init__(self, sr_in: int, sr_out: int = SAMPLE_RATE, block: int = BLOCK_SIZE):
        self.sr_in, self.sr_out, self.block = sr_in, sr_out, block
        self._in = np.empty(0, dtype=np.float32)
        self._out = np.empty(0, dtype=np.int16)

    def process(self, data: np.ndarray) -> list[np.ndarray]:
        self._in = np.concatenate([self._in, data.astype(np.float32)])
        n_out = int(len(self._in) * self.sr_out / self.sr_in)
        if n_out < 1:
            return []
        idx = np.linspace(0.0, len(self._in) - 1, n_out, dtype=np.float32)
        resampled = np.interp(idx, np.arange(len(self._in), dtype=np.float32), self._in)
        consumed = int(np.floor(idx[-1]))  # keep the tail for next round
        self._in = self._in[consumed:]
        self._out = np.concatenate(
            [self._out, np.clip(resampled, -32768, 32767).astype(np.int16)]
        )
        chunks, n = [], len(self._out) // self.block
        for i in range(n):
            chunks.append(self._out[i * self.block : (i + 1) * self.block].copy())
        self._out = self._out[n * self.block :]
        return chunks


@contextmanager
def input_stream(callback, device: str | int | None = None):
    """Yield with a 16 kHz mono int16 stream running; frames → callback.

    Devices that refuse 16 kHz are captured at their default rate and resampled.
    """
    try:
        import sounddevice as sd
    except (ImportError, OSError) as e:
        raise VoiceError(
            f"sounddevice unavailable ({e}) — pip install -e '.[voice]' and ensure "
            f"PortAudio is installed (Fedora: sudo dnf install portaudio)"
        ) from e

    def _cb(indata, frames, time_info, status):
        callback(indata.copy())

    dev = None if device in (None, "") else (int(device) if str(device).isdigit() else device)
    try:  # native 16 kHz — PipeWire handles this fine
        stream = sd.InputStream(
            samplerate=SAMPLE_RATE, channels=1, dtype="int16",
            blocksize=BLOCK_SIZE, device=dev, callback=_cb,
        )
        stream.start()
        try:
            yield
        finally:
            stream.stop()
            stream.close()
        return
    except Exception:
        pass

    # fallback: capture at the device default rate, resample to 16 kHz
    try:
        info = sd.query_devices(kind="input")
        sr_in = int(info["default_samplerate"])
        rs = _Resampler(sr_in)

        def _cb_rs(indata, frames, time_info, status):
            for chunk in rs.process(indata[:, 0]):
                callback(chunk)

        stream = sd.InputStream(samplerate=sr_in, channels=1, dtype="int16",
                                device=dev, callback=_cb_rs)
        stream.start()
        try:
            yield
        finally:
            stream.stop()
            stream.close()
    except Exception as e:
        raise VoiceError(
            f"could not open an input device ({e}) — check `pactl info` and "
            f"`python -m mtrxai.voice.calibrate --list-devices`"
        ) from e


def list_devices() -> str:
    try:
        import sounddevice as sd
    except (ImportError, OSError) as e:
        return f"sounddevice unavailable: {e}"
    lines = ["input devices:"]
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0:
            lines.append(f"  [{i}] {d['name']} ({d['default_samplerate']} Hz)")
    lines.append("set `input_device` in settings.toml [voice] to the [index] you want")
    return "\n".join(lines)


def play_int16(data: np.ndarray, samplerate: int, stop=None) -> None:
    """Blocking playback; a concurrent sd.stop() (barge-in) unblocks it."""
    import sounddevice as sd

    if stop is not None and stop.is_set():
        return
    try:
        sd.play(data, samplerate, blocking=True)
    except Exception:
        sd.stop()
        raise
