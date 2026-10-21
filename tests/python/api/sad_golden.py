"""Golden vectors of the ADF sound activity detector, from `sad_reference`.

Run `uv run python -m tests.python.api.sad_golden` from the repository root to rewrite
`tests/data/periph/mdf/sad_*.json`. Inputs come from a fixed-seed integer generator, so the
files are reproducible on any host.
"""

from __future__ import annotations

import json
from collections.abc import Callable, Iterator
from dataclasses import asdict
from pathlib import Path

from .sad_reference import SadConfig, sad

VECTORS = Path(__file__).resolve().parents[2] / "data" / "periph" / "mdf"

type Envelope = Callable[[int], int]


def generator(seed: int) -> Iterator[int]:
    """64-bit linear congruential generator, upper 31 bits."""
    state = seed
    while True:
        state = (state * 6364136223846793005 + 1442695040888963407) % 2**64
        yield state >> 33


def signal(seed: int, frames: int, frame: int, amplitude: Envelope) -> list[int]:
    """Uniform noise of the frame's amplitude in PCM[23:8], random low bits.

    An amplitude of -1 gives a frame of the most negative 24-bit sample.
    """
    draw = generator(seed)
    out: list[int] = []
    for index in range(frames):
        peak = amplitude(index)
        for _ in range(frame):
            low = next(draw) & 0xFF
            if peak < 0:
                out.append(-(1 << 23))
                continue
            top = next(draw) % (2 * peak + 1) - peak
            out.append(top * 256 + low)
    return out


def bursts(base: int, spans: list[tuple[int, int, int]]) -> Envelope:
    """Amplitude `base`, or the amplitude of the first span [start, end) holding the frame."""

    def at(index: int) -> int:
        for start, end, peak in spans:
            if start <= index < end:
                return peak
        return base

    return at


def ramp(index: int) -> int:
    """Ambient noise rising from 200 to 3000 over frames 40 to 200, then back down."""
    if index < 40:
        return 200
    if index < 200:
        return 200 + (index - 40) * 2800 // 160
    if index < 240:
        return 3000
    return max(200, 3000 - (index - 240) * 100)


def speech(index: int) -> int:
    """Background 400, loud bursts, a medium tail and a medium burst, then silence."""
    return bursts(
        400,
        [(60, 80, 6000), (80, 110, 1800), (150, 170, 1800), (200, 205, 6000), (250, 300, 0)],
    )(index)


def run(config: SadConfig, seed: int, frames: int, amplitude: Envelope) -> dict[str, object]:
    """One run: its config, its input and the reference's frames."""
    pcm = signal(seed, frames, config.frame, amplitude)
    return {
        "config": asdict(config),
        "pcm": "".join(f"{x & 0xFFFFFF:06x}" for x in pcm),
        "frames": [[f.sdlvl, f.anlvl, int(f.state), int(f.changed)] for f in sad(pcm, config)],
    }


def vectors() -> dict[str, dict[str, object]]:
    """Every vector file by name."""
    noise = SadConfig(
        sadmod=0, frsize=0, lfrnb=1, hgovr=0, anslp=3, snthr=3, hysten=False, anmin=120
    )
    hysteresis = SadConfig(
        sadmod=0, frsize=0, lfrnb=1, hgovr=0, anslp=3, snthr=4, hysten=True, anmin=120
    )
    no_hysteresis = SadConfig(
        sadmod=0, frsize=0, lfrnb=1, hgovr=0, anslp=3, snthr=4, hysten=False, anmin=120
    )
    fixed = SadConfig(
        sadmod=1, frsize=1, lfrnb=0, hgovr=1, anslp=0, snthr=2, hysten=False, anmin=500
    )
    fixed_hysteresis = SadConfig(
        sadmod=1, frsize=1, lfrnb=0, hgovr=1, anslp=0, snthr=2, hysten=True, anmin=500
    )
    fixed_input = bursts(300, [(30, 50, 5000), (50, 70, 2500), (90, 95, -1), (120, 140, 2500)])
    ambient = SadConfig(
        sadmod=2, frsize=0, lfrnb=0, hgovr=0, anslp=7, snthr=0, hysten=False, anmin=300
    )
    ambient_slow = SadConfig(
        sadmod=3, frsize=0, lfrnb=4, hgovr=2, anslp=5, snthr=1, hysten=False, anmin=200
    )
    return {
        "sad_noise": {
            "name": "noise",
            "runs": [
                run(noise, 1, 300, speech),
                run(hysteresis, 1, 300, speech),
                run(no_hysteresis, 1, 300, speech),
            ],
        },
        "sad_fixed": {
            "name": "fixed",
            "runs": [
                run(fixed, 2, 160, fixed_input),
                run(fixed_hysteresis, 2, 160, fixed_input),
            ],
        },
        "sad_ambient": {
            "name": "ambient",
            "runs": [run(ambient, 3, 300, ramp), run(ambient_slow, 4, 300, ramp)],
        },
    }


def main() -> None:
    """Writes every vector file."""
    for name, content in vectors().items():
        (VECTORS / f"{name}.json").write_text(json.dumps(content, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
