"""Wake-word calibration and self-test.

    python -m mtrxai.voice.calibrate --list-devices
    python -m mtrxai.voice.calibrate --selftest
    python -m mtrxai.voice.calibrate --seconds 30
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

from ..config import find_project_root, load_settings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m mtrxai.voice.calibrate")
    ap.add_argument("--model", help="wake model path (default: settings [voice].wake_model)")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--device", help="input device index/name")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="load model + score silence, no mic")
    args = ap.parse_args(argv)

    if args.list_devices:
        from .audio import list_devices

        print(list_devices())
        return 0

    voice = load_settings(find_project_root(None))["voice"]
    model = Path(args.model) if args.model else Path(voice["wake_model"])

    from .wake import WakeWord

    if args.selftest:
        ww = WakeWord(model, threshold=1.1)  # never triggers; we only want scores
        zeros = np.zeros(1280, dtype=np.int16)
        scores = [ww.score(zeros) for _ in range(12)]  # ~1 s of silence
        print(f"wake model : {model}")
        print(f"scores/s on silence: {', '.join(f'{s:.3f}' for s in scores)}")
        print(f"max silence score   : {max(scores):.3f}  (should be ≈ 0)")
        print("OK — the custom wake model loads and runs.")
        return 0

    ww = WakeWord(model, threshold=1.1)
    stats = {"max": 0.0, "frames": 0}

    def cb(frame: np.ndarray) -> None:
        s = ww.score(frame)
        stats["max"] = max(stats["max"], s)
        stats["frames"] += 1
        print(f"\rscore={s:5.3f}  max={stats['max']:5.3f}  ({stats['frames']} frames)",
              end="", flush=True)

    from .audio import input_stream

    dev = args.device if args.device else (voice.get("input_device") or None)
    print(f"Say “hey matrix” several times, then a few similar-sounding phrases.")
    print(f"Calibrating for {args.seconds:.0f}s on model {model.name} (ctrl+c to stop early)\n")
    try:
        with input_stream(cb, device=dev):
            time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    print(f"\n\nmax score seen      : {stats['max']:.3f}")
    print(f"frames processed    : {stats['frames']}")
    print(
        "Pick wake_threshold above the impostor peaks but below your real hits\n"
        "(typically 0.35–0.6), then set it in settings.toml [voice]."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
