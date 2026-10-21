"""Golden vectors of the MDF filter chain and modulator, from `mdf_reference`.

Run `uv run python -m tests.python.api.mdf_golden` from the repository root to rewrite
`tests/data/periph/mdf/`. Each `chain_<name>.json` holds runs of a config, its input bits and
the expected PCM and SATF flags. Each `modulator_<name>.json` holds PCM input, bit counts per
sample and the expected bits.

Encodings: `config` holds the `ChainConfig` fields, `order` as a string, "fastsinc" or "1" to
"5", `hpf` and `int_value` null when off. `bits` is `{"hex": ..., "count": n}`, bit i in byte
i / 8 at weight 0x80 >> i % 8, or `{"runs": [[bit, count], ...]}`. `pcm` and `saturated` are
run-length `[[value, count], ...]`, `saturated` values 0 or 1.
"""

from __future__ import annotations

import dataclasses
import json
import math
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from .mdf_reference import ChainConfig, modulate, run_chain

DATA = Path(__file__).resolve().parents[2] / "data" / "periph" / "mdf"
BIT_RATE = 2_048_000
HOLD = 16
SATURATING = ChainConfig(order=5, decimation=32, scale=0x02)
DISCARD_CARRY = ChainConfig(order=5, decimation=32, scale=0x02, discard=32)
OVERLOAD = ([32767] * 8 + [0] * 8, [1024] * 16)
INT_CARRY = ChainConfig(order=5, decimation=32, scale=0x02, int_value=3, int_div=4)

type Json = dict[str, Any]


def prbs7(count: int) -> list[int]:
    """PRBS7, x^7 + x^6 + 1, seed 0x7F."""
    state = 0x7F
    bits: list[int] = []
    for _ in range(count):
        bit = ((state >> 6) ^ (state >> 5)) & 1
        state = ((state << 1) | bit) & 0x7F
        bits.append(bit)
    return bits


def tone(level_db: float, frequency: float, rate: float, count: int) -> list[int]:
    """@p count 16-bit samples of a sine at @p level_db dBFS sampled at @p rate."""
    amplitude = 32767 * 10 ** (level_db / 20)
    return [round(amplitude * math.sin(2 * math.pi * frequency * i / rate)) for i in range(count)]


def sine_bits(level_db: float, frequency: float, bit_rate: int, count: int) -> list[int]:
    """@p count modulated bits of a sine, PCM held for `HOLD` bits each."""
    samples = count // HOLD
    pcm = tone(level_db, frequency, bit_rate / HOLD, samples)
    return modulate(pcm, [HOLD] * samples)


def step_bits(count: int) -> list[int]:
    """@p count modulated bits of silence for a quarter, then DC at half full scale."""
    samples = count // HOLD
    pcm = [0 if i < samples // 4 else 16384 for i in range(samples)]
    return modulate(pcm, [HOLD] * samples)


def runs_of[T](values: Sequence[T]) -> list[tuple[T, int]]:
    """Run-length encoding of @p values."""
    out: list[tuple[T, int]] = []
    for value in values:
        if out and out[-1][0] == value:
            out[-1] = (value, out[-1][1] + 1)
        else:
            out.append((value, 1))
    return out


def expand[T](runs: Sequence[tuple[T, int]]) -> list[T]:
    """Inverse of `runs_of`."""
    return [value for value, count in runs for _ in range(count)]


def pack(bits: Sequence[int]) -> Json:
    """@p bits as `{"hex", "count"}`."""
    data = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        if bit:
            data[i // 8] |= 0x80 >> (i % 8)
    return {"hex": data.hex(), "count": len(bits)}


def unpack(encoded: Json) -> list[int]:
    """Inverse of `pack`, and of the `runs` form."""
    if "runs" in encoded:
        return expand([(int(bit), int(count)) for bit, count in encoded["runs"]])
    data = bytes.fromhex(encoded["hex"])
    return [(data[i // 8] >> (7 - i % 8)) & 1 for i in range(int(encoded["count"]))]


def decode_config(encoded: Json) -> ChainConfig:
    """The ChainConfig of a vector's `config`, whose `order` is a string."""
    fields: dict[str, Any] = dict(encoded)
    order = fields["order"]
    fields["order"] = order if order == "fastsinc" else int(order)
    return ChainConfig(**fields)


def chain_run(config: ChainConfig, bits: Sequence[int], *, as_runs: bool = False) -> Json:
    """One run of a chain vector."""
    output = run_chain(bits, config)
    encoded: Json = {"runs": [list(r) for r in runs_of(bits)]} if as_runs else pack(bits)
    return {
        "config": dataclasses.asdict(config) | {"order": str(config.order)},
        "bits": encoded,
        "pcm": [list(r) for r in runs_of(output.pcm)],
        "saturated": [[int(flag), count] for flag, count in runs_of(output.saturated)],
    }


def chain_cases() -> dict[str, list[Json]]:
    """Every chain vector, by name."""
    pdm_16k = ChainConfig(order=5, decimation=32, scale=0x2C, rsflt=True, hpf=2)
    quiet = sine_bits(-12, 1000, BIT_RATE, 512 * 128)
    loud = sine_bits(-6, 1000, BIT_RATE, 512 * 128)
    half = sine_bits(-6, 1000, BIT_RATE // 2, 16384)
    noise = prbs7(4096)
    step = step_bits(2048 * 16)
    cases: dict[str, list[Json]] = {
        "pdm_16k": [chain_run(pdm_16k, quiet)],
        "pdm_16k_loud": [chain_run(pdm_16k, loud)],
        "int_bypass": [chain_run(ChainConfig(order=4, decimation=8, int_div=128), noise)],
        "fastsinc": [chain_run(ChainConfig(order="fastsinc", decimation=16), noise)],
        "d2": [chain_run(ChainConfig(order=1, decimation=2), prbs7(1024))],
        "d512": [chain_run(ChainConfig(order=1, decimation=512), prbs7(512 * 64))],
        "wrap": [
            chain_run(
                ChainConfig(order=5, decimation=32, scale=0x20),
                [1] * BIT_RATE,
                as_runs=True,
            )
        ],
        "scale_all": [
            chain_run(ChainConfig(order=4, decimation=8, scale=code), prbs7(512))
            for code in [*range(0x00, 0x19), *range(0x20, 0x30)]
        ],
        "sat": [chain_run(ChainConfig(order=5, decimation=32, scale=0x02), loud[:16384])],
        "rsflt_dec1": [
            chain_run(ChainConfig(order=5, decimation=16, rsflt=True, rsflt_decimate=False), half)
        ],
        "offset": [
            chain_run(ChainConfig(order=3, decimation=16, offset=1000), noise),
            chain_run(ChainConfig(order=5, decimation=32, scale=0x20, offset=-(1 << 25)), half),
        ],
        "int": [
            chain_run(
                ChainConfig(order=3, decimation=16, scale=0x04, int_value=8, int_div=div), half
            )
            for div in (128, 32, 4, 1)
        ]
        + [
            chain_run(
                ChainConfig(order=4, decimation=16, scale=0x04, int_value=128, int_div=1), step
            ),
            chain_run(INT_CARRY, loud[:16384]),
        ],
        "discard_skip": [
            chain_run(ChainConfig(order=4, decimation=8, discard=5, skip=3), noise),
            chain_run(
                ChainConfig(order=5, decimation=16, rsflt=True, hpf=1, discard=5, skip=3), half
            ),
            chain_run(
                ChainConfig(order=3, decimation=16, scale=0x04, discard=5, int_value=4, int_div=1),
                noise,
            ),
            chain_run(DISCARD_CARRY, loud[:16384]),
        ],
    }
    for order in (1, 2, 3, 4, 5):
        cases[f"sinc{order}"] = [chain_run(ChainConfig(order=order, decimation=16), noise)]
    for hpfc in range(4):
        cases[f"hpf{hpfc}"] = [
            chain_run(ChainConfig(order=4, decimation=16, scale=0x02, hpf=hpfc), step)
        ]
    return cases


def modulator_cases() -> dict[str, Json]:
    """Every modulator vector, by name: a sine then a ramp at four sample rates, and an
    overload then silence, whose recovery depends on the integrator clamps."""
    ramp = [-30000 + i * 60000 // 127 for i in range(128)]
    cases: dict[str, Json] = {}
    for name, rate in (("16k", 16_000), ("32k", 32_000), ("44k1", 44_100), ("64k", 64_000)):
        pcm = tone(-6, 1000, rate, 128) + ramp
        counts = [(i + 1) * BIT_RATE // rate - i * BIT_RATE // rate for i in range(len(pcm))]
        cases[name] = {
            "pcm16": pcm,
            "bits_per_sample": counts,
            "bits": pack(modulate(pcm, counts)),
        }
    pcm, counts = OVERLOAD
    cases["overload"] = {
        "pcm16": pcm,
        "bits_per_sample": counts,
        "bits": pack(modulate(pcm, counts)),
    }
    return cases


def render(value: Json) -> str:
    """The file text of a vector."""
    return json.dumps(value, separators=(",", ":")) + "\n"


def generated() -> dict[str, str]:
    """File name to text of every vector."""
    files = {
        f"chain_{name}.json": render({"name": name, "runs": runs})
        for name, runs in chain_cases().items()
    }
    files.update(
        {
            f"modulator_{name}.json": render({"name": name, "modulator": case})
            for name, case in modulator_cases().items()
        }
    )
    return files


def main() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    for stale in DATA.glob("*.json"):
        stale.unlink()
    for name, text in generated().items():
        (DATA / name).write_text(text)


if __name__ == "__main__":
    main()
