"""One-shot voice asset setup and environment check.

    python -m mtrxai.voice.setup                # check + fix what's safe
    python -m mtrxai.voice.setup --with-stt     # also pre-download the whisper model
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from ..config import find_project_root, load_settings

OK, FAIL, WARN = "✓", "✗", "⚠"


def _check(name: str, fn) -> bool:
    try:
        detail = fn()
        print(f"{OK} {name}" + (f" — {detail}" if detail else ""))
        return True
    except Exception as e:  # noqa: BLE001 — report, don't crash
        print(f"{FAIL} {name} — {e}")
        return False


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m mtrxai.voice.setup")
    ap.add_argument("--with-stt", action="store_true", help="pre-download the whisper model")
    args = ap.parse_args(argv)

    settings = load_settings(find_project_root(None))
    voice = settings["voice"]
    root = settings["project_root"]
    results: list[bool] = []

    def wake_model() -> str:
        src = Path.home() / "mtrxAI.onnx"
        dst = root / "assets" / "wake" / "mtrxAI.onnx"
        if not dst.is_file() and src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, dst)
        if not dst.is_file():
            raise RuntimeError(f"not found: {dst} (and no {src} to copy)")
        return str(dst)

    def openwakeword() -> str:
        import openwakeword  # noqa: F401
        from openwakeword.utils import download_models

        download_models()
        return "feature models cached"

    def piper_bin() -> str:
        which = shutil.which("piper")
        if not which:
            raise RuntimeError("piper CLI missing — pip install -e '.[voice]'")
        return which

    def piper_voice() -> str:
        name = str(voice["tts"]).partition(":")[2] or "en_US-amy-medium"
        vdir = Path(voice["voices_dir"]).expanduser()
        vdir.mkdir(parents=True, exist_ok=True)
        model = vdir / f"{name}.onnx"
        if model.is_file():
            return str(model)
        print(f"  … downloading piper voice {name}")
        proc = subprocess.run(
            [sys.executable, "-m", "piper.download_voices", name, "--download-dir", str(vdir)],
            capture_output=True, text=True, timeout=600,
        )
        if proc.returncode != 0 or not model.is_file():
            raise RuntimeError(
                f"auto-download failed — manual: download {name}.onnx (+ .onnx.json) "
                f"from huggingface.co/rhasspy/piper-voices into {vdir}"
            )
        return str(model)

    def sounddevice() -> str:
        import sounddevice as sd

        n = sum(1 for d in sd.query_devices() if d["max_input_channels"] > 0)
        if n == 0:
            raise RuntimeError("no input device — check `pactl info` / PipeWire")
        return f"{n} input device(s)"

    def webrtcvad() -> str:
        import webrtcvad  # noqa: F401

        return "ok"

    def whisper_stt() -> str:
        from faster_whisper import WhisperModel

        model = str(voice["stt"]).partition(":")[2] or "base.en"
        WhisperModel(model, device="cpu", compute_type="int8")
        return f"{model} cached"

    print("mtrxAI voice setup\n------------------")
    results.append(_check("wake model file", wake_model))
    results.append(_check("openwakeword + feature models", openwakeword))
    results.append(_check("webrtcvad", webrtcvad))
    results.append(_check("sounddevice / PortAudio input", sounddevice))
    results.append(_check("piper binary", piper_bin))
    results.append(_check("piper voice", piper_voice))
    if args.with_stt:
        results.append(_check("faster-whisper model", whisper_stt))
    else:
        print(f"{WARN} skipping whisper model download (re-run with --with-stt to prefetch)")

    if all(results):
        print("\nAll set — enable voice with ctrl+v in the TUI or `mtrxai --voice`.")
        return 0
    print("\nSome checks failed — fix the ✗ lines above and re-run.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
