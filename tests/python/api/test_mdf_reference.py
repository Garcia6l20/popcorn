"""The MDF reference against RM0456 closed forms, and the committed golden vectors."""

from __future__ import annotations

import cmath
import dataclasses
import json
import math
from collections.abc import Callable, Sequence
from typing import Literal

import pytest

from .mdf_golden import (
    BIT_RATE,
    DATA,
    DISCARD_CARRY,
    HOLD,
    INT_CARRY,
    OVERLOAD,
    SATURATING,
    decode_config,
    expand,
    generated,
    prbs7,
    sine_bits,
    step_bits,
    tone,
    unpack,
)
from .mdf_reference import (
    PCM_MAX,
    PCM_MIN,
    RSFLT_CUTOFF,
    RSFLT_SECTIONS,
    ChainConfig,
    Order,
    cic,
    hpf_response,
    modulate,
    rsflt_design,
    rsflt_response,
    run_chain,
    scale_db,
    scale_factor,
    sinc_response,
    wrap,
)

SINC_ORDERS: tuple[Literal[1, 2, 3, 4, 5], ...] = (1, 2, 3, 4, 5)
TABLE_375_COLUMNS: tuple[Order, ...] = (1, 2, "fastsinc", 3, 4, 5)
TABLE_375: dict[int, tuple[tuple[int | None, ...], tuple[int | None, ...]]] = {
    4: ((3, 5, 6, 7, 9, 11), (14, 16, 17, 18, 20, 22)),
    8: ((4, 7, 8, 10, 13, 16), (15, 18, 19, 21, 24, None)),
    12: ((5, 9, 9, 12, 16, 19), (16, 20, 21, 23, None, None)),
    16: ((5, 9, 10, 13, 17, 21), (16, 20, 21, 24, None, None)),
    24: ((6, 11, 12, 15, 20, 24), (17, 22, 23, None, None, None)),
    32: ((6, 11, 12, 16, 21, 26), (17, 22, 23, None, None, None)),
    64: ((7, 13, 14, 19, 25, None), (18, 24, 25, None, None, None)),
    128: ((8, 15, 16, 22, None, None), (19, 26, None, None, None, None)),
    256: ((9, 17, 18, 25, None, None), (20, None, None, None, None, None)),
    512: ((10, 19, 20, None, None, None), (21, None, None, None, None, None)),
}
TABLE_375_MISPRINTS = {(12, "fastsinc", 1)}
TABLE_377 = {
    0x20: -48.2, 0x21: -44.6, 0x22: -42.1, 0x23: -38.6, 0x24: -36.1, 0x25: -32.6,
    0x26: -30.1, 0x27: -26.6, 0x28: -24.1, 0x29: -20.6, 0x2A: -18.1, 0x2B: -14.5,
    0x2C: -12.0, 0x2D: -8.5, 0x2E: -6.0, 0x2F: -2.5, 0x00: 0.0, 0x01: 3.5,
    0x02: 6.0, 0x03: 9.5, 0x04: 12.0, 0x05: 15.6, 0x06: 18.1, 0x07: 21.6,
    0x08: 24.1, 0x09: 27.6, 0x0A: 30.1, 0x0B: 33.6, 0x0C: 36.1, 0x0D: 39.6,
    0x0E: 42.1, 0x0F: 45.7, 0x10: 48.2, 0x11: 51.7, 0x12: 54.2, 0x13: 57.7,
    0x14: 60.2, 0x15: 63.7, 0x16: 66.2, 0x17: 69.7, 0x18: 72.2,
}  # fmt: skip
HPF_FRACTIONS = (0.000625, 0.00125, 0.0025, 0.0095)
RM_GLOBAL_CUTOFF_HZ = 7061
MODULATOR_SNR_DB = 80


def bisect(response: Callable[[float], float], low: float, high: float) -> float:
    """Frequency between @p low and @p high where @p response crosses -3 dB."""
    target = 10 ** (-3 / 20)
    rising = response(low) < response(high)
    for _ in range(200):
        middle = (low + high) / 2
        if (response(middle) < target) == rising:
            low = middle
        else:
            high = middle
    return low


def cic_response(frequency: float, rate: float, order: int, decimation: int) -> float:
    angle = math.pi * frequency / rate
    return abs(math.sin(decimation * angle) / (decimation * math.sin(angle))) ** order


def fft(values: Sequence[complex]) -> list[complex]:
    if len(values) == 1:
        return list(values)
    even = fft(values[0::2])
    odd = fft(values[1::2])
    half = len(values) // 2
    twiddled = [cmath.exp(-2j * math.pi * k / len(values)) * odd[k] for k in range(half)]
    return [even[k] + twiddled[k] for k in range(half)] + [
        even[k] - twiddled[k] for k in range(half)
    ]


def test_the_committed_vectors_are_what_the_reference_generates() -> None:
    expected = generated()
    committed = {path.name: path.read_text() for path in DATA.glob("*.json")}
    assert sorted(committed) == sorted(expected)
    for name, text in expected.items():
        assert committed[name] == text, name


@pytest.mark.parametrize("order", SINC_ORDERS)
@pytest.mark.parametrize("decimation", [2, 3, 16, 32])
def test_sinc_impulse_response(order: Literal[1, 2, 3, 4, 5], decimation: int) -> None:
    response = sinc_response(order, decimation)
    assert len(response) == order * (decimation - 1) + 1
    assert sum(response) == decimation**order
    assert response == response[::-1]
    rebuilt = [0] * len(response)
    for phase in range(decimation):
        impulse = [0] * phase + [1] + [0] * (len(response) + decimation)
        for m, value in enumerate(cic(impulse, order, decimation)):
            tap = (m + 1) * decimation - 1 - phase
            if 0 <= tap < len(response):
                rebuilt[tap] = value
            else:
                assert value == 0
    assert rebuilt == response


def test_fastsinc_is_sinc2_times_one_plus_z_minus_2d() -> None:
    decimation = 8
    sinc2 = sinc_response(2, decimation)
    expected = [0] * (len(sinc2) + 2 * decimation)
    for i, value in enumerate(sinc2):
        expected[i] += value
        expected[i + 2 * decimation] += value
    impulse = [1] + [0] * (len(expected) + decimation)
    outputs = cic(impulse, "fastsinc", decimation)
    taps = [(m + 1) * decimation - 1 for m in range(len(outputs))]
    assert outputs == [expected[t] if t < len(expected) else 0 for t in taps]
    assert sum(expected) == 2 * decimation**2


@pytest.mark.parametrize("order", SINC_ORDERS)
def test_the_first_outputs_see_zero_history(order: Literal[1, 2, 3, 4, 5]) -> None:
    decimation = 4
    response = sinc_response(order, decimation)
    outputs = cic([1] * (decimation * (order + 3)), order, decimation)
    for m, value in enumerate(outputs):
        assert value == sum(response[: (m + 1) * decimation])
    assert outputs[-1] == decimation**order


def test_table_375_data_sizes() -> None:
    for decimation, columns in TABLE_375.items():
        for dsin, sizes in zip((1, 12), columns, strict=True):
            for order, size in zip(TABLE_375_COLUMNS, sizes, strict=True):
                if size is None or (decimation, order, dsin) in TABLE_375_MISPRINTS:
                    continue
                gain = sum(cic([1] * decimation * 12, order, decimation)[-1:])
                assert math.ceil(math.log2(gain) + dsin) == size, (decimation, order, dsin)


def test_table_375_misprint_is_one_bit_short() -> None:
    gain = cic([1] * 12 * 12, "fastsinc", 12)[-1]
    assert gain == 2 * 12**2
    assert math.ceil(math.log2(gain) + 1) == 10


def test_table_377() -> None:
    for code, gain in TABLE_377.items():
        assert scale_db(code) == pytest.approx(gain, abs=0.06), hex(code)
    for code in (*range(0x19, 0x20), *range(0x30, 0x40)):
        with pytest.raises(ValueError):
            scale_factor(code)


def test_a_sinc5_d32_full_scale_stream_wraps_at_26_bits() -> None:
    assert cic([1] * 32 * 6, 5, 32)[-1] == 1 << 25
    assert wrap(1 << 25, 26) == -(1 << 25)
    out = run_chain([1] * 32 * 8, ChainConfig(order=5, decimation=32, scale=0x20))
    assert out.pcm[-1] == -(1 << 25) >> 8
    assert not any(out.saturated)


def test_sat_clamps_after_scale_and_flags_the_sample() -> None:
    out = run_chain([1] * 32 * 8, ChainConfig(order=5, decimation=32, offset=-1))
    assert out.pcm[-1] == PCM_MIN
    assert out.saturated[-1]
    out = run_chain([1] * 16 * 8, ChainConfig(order=5, decimation=16, scale=0x08))
    assert out.pcm[-1] == PCM_MAX
    assert out.saturated[-1]


@pytest.mark.parametrize("hpfc", range(4))
def test_hpf_cutoffs_are_within_5_percent_of_the_rm(hpfc: int) -> None:
    cutoff = bisect(lambda f: hpf_response(f, 1.0, hpfc), 1e-7, 0.25)
    assert cutoff == pytest.approx(HPF_FRACTIONS[hpfc], rel=0.05)


def test_the_rsflt_coefficients_are_the_design_at_the_cutoff() -> None:
    assert rsflt_design(RSFLT_CUTOFF) == RSFLT_SECTIONS


def test_cic5_d16_and_rsflt_put_the_global_cutoff_on_the_rm_7061_hz() -> None:
    rate = 1_024_000
    rsflt_rate = rate / 16

    def response(frequency: float) -> float:
        return cic_response(frequency, rate, 5, 16) * rsflt_response(frequency, rsflt_rate)

    cutoff = bisect(response, 100, rsflt_rate / 2 - 1)
    assert cutoff == pytest.approx(RM_GLOBAL_CUTOFF_HZ, rel=0.03)


def test_the_rsflt_has_gain_3_and_takes_a_22_bit_input_unsaturated() -> None:
    config = ChainConfig(order=1, decimation=2, offset=-(1 << 21) - 2, rsflt=True)
    out = run_chain([0] * 2 * 400, config)
    assert out.pcm[-1] == pytest.approx(3 * ((1 << 21) + 2 - 2), abs=4)
    assert not any(out.saturated)
    config = dataclasses.replace(config, offset=-(1 << 22))
    assert any(run_chain([0] * 2 * 400, config).saturated)


def test_int_bypass_ignores_intdiv_and_equals_the_int_less_chain() -> None:
    bits = prbs7(4096)
    base = ChainConfig(order=4, decimation=8)
    outputs = [run_chain(bits, dataclasses.replace(base, int_div=div)) for div in (128, 32, 4, 1)]
    assert all(out == outputs[0] for out in outputs)
    assert outputs[0].pcm == [wrap(v, 26) for v in cic([1 if b else -1 for b in bits], 4, 8)]


@pytest.mark.parametrize(("div", "amount"), [(128, 7), (32, 5), (4, 2), (1, 0)])
def test_int_sums_intval_plus_one_samples_and_shifts(div: int, amount: int) -> None:
    bits = prbs7(4096)
    base = ChainConfig(order=3, decimation=16, scale=0x04)
    plain = run_chain(bits, base).pcm
    out = run_chain(bits, dataclasses.replace(base, int_value=8, int_div=div)).pcm
    assert out == [sum(plain[i : i + 8]) >> amount for i in range(0, len(plain) - 7, 8)]


def test_discard_drops_samples_after_the_skipped_bits() -> None:
    bits = prbs7(4096)
    base = ChainConfig(order=4, decimation=8)
    out = run_chain(bits, dataclasses.replace(base, discard=5, skip=3)).pcm
    assert out == run_chain(bits[3:], base).pcm[5:]


def test_discard_runs_before_the_int() -> None:
    bits = prbs7(4096)
    base = ChainConfig(order=3, decimation=16, scale=0x04, discard=5)
    plain = run_chain(bits, base).pcm
    out = run_chain(bits, dataclasses.replace(base, int_value=4, int_div=1)).pcm
    assert out == [sum(plain[i : i + 4]) for i in range(0, len(plain) - 3, 4)]


def test_the_loud_pdm_16k_vector_saturates_on_its_peaks_and_the_quiet_one_never() -> None:
    for name, expect in (("chain_pdm_16k_loud.json", True), ("chain_pdm_16k.json", False)):
        run = committed_run(name)
        flags = [bool(f) for f in expand([(int(f), int(c)) for f, c in run["saturated"]])]
        pcm = expand([(int(v), int(c)) for v, c in run["pcm"]])
        assert any(flags) == expect
        if expect:
            peak = max(abs(v) for v in pcm)
            assert all(abs(v) >= peak // 2 for v, f in zip(pcm, flags, strict=True) if f)


def committed_run(name: str) -> dict[str, list[list[int]]]:
    return json.loads((DATA / name).read_text())["runs"][0]


def test_the_vectors_decode_to_the_reference_output() -> None:
    run = json.loads((DATA / "chain_discard_skip.json").read_text())["runs"][0]
    config = decode_config(run["config"])
    out = run_chain(unpack(run["bits"]), config)
    assert out.pcm == expand([(int(v), int(c)) for v, c in run["pcm"]])


def test_the_modulator_reaches_80_db_snr_through_cic5_d32_and_rsflt() -> None:
    size = 4096
    settle = 256
    bin_ = 257
    pcm_rate = BIT_RATE // HOLD
    count = (size + settle) * 128 // HOLD
    pcm = tone(-12, bin_ * 16_000 / size, pcm_rate, count)
    bits = modulate(pcm, [HOLD] * count)
    out = run_chain(bits, ChainConfig(order=5, decimation=32, scale=0x27, rsflt=True))
    spectrum = fft([complex(v) for v in out.pcm[settle : settle + size]])
    power = [abs(c) ** 2 for c in spectrum[: size // 2]]
    signal = sum(power[bin_ - 1 : bin_ + 2])
    noise = sum(power[2 : 7000 * size // 16_000]) - signal
    assert 10 * math.log10(signal / noise) > MODULATOR_SNR_DB


@pytest.mark.parametrize("level", [29491, -29491])
def test_the_modulator_tracks_dc_at_0_9_full_scale(level: int) -> None:
    bits = modulate([level] * 64, [1024] * 64)
    for start in range(4096, len(bits), 4096):
        mean = sum(1 if b else -1 for b in bits[start : start + 4096]) / 4096
        assert mean == pytest.approx(level / 32768, abs=2 / 4096)


def test_the_modulator_overload_stays_bounded() -> None:
    bits = modulate([32767] * 64, [1024] * 64)
    assert sum(bits[4096:]) / len(bits[4096:]) > 0.99


def unclamped(pcm16: Sequence[int], bits_per_sample: Sequence[int]) -> list[int]:
    i1 = 0
    i2 = 0
    bits: list[int] = []
    for x, count in zip(pcm16, bits_per_sample, strict=True):
        for _ in range(count):
            fb = 32768 if i2 >= 0 else -32768
            i2 += i1 - 2 * fb
            i1 += x - fb
            bits.append(1 if fb > 0 else 0)
    return bits


def test_the_clamps_shape_the_recovery_after_an_overload() -> None:
    clamped = modulate(*OVERLOAD)
    loose = unclamped(*OVERLOAD)
    assert sum(a != b for a, b in zip(clamped, loose, strict=True)) > 1000


def test_the_int_keeps_its_low_24_bits() -> None:
    bits = step_bits(2048 * 16)
    base = ChainConfig(order=4, decimation=16, scale=0x04)
    plain = run_chain(bits, base).pcm
    out = run_chain(bits, dataclasses.replace(base, int_value=128, int_div=1)).pcm
    sums = [sum(plain[i : i + 128]) for i in range(0, len(plain) - 127, 128)]
    assert out == [wrap(total, 24) for total in sums]
    assert any(total > PCM_MAX for total in sums)


def test_satf_carries_through_discard_and_int() -> None:
    loud = sine_bits(-6, 1000, BIT_RATE, 16384)
    flags = run_chain(loud, SATURATING).saturated
    assert any(flags[:32])
    assert not flags[32]
    assert run_chain(loud, DISCARD_CARRY).saturated[0]
    assert flags[30] and not flags[32]
    assert run_chain(loud, INT_CARRY).saturated[10]


@pytest.mark.parametrize(
    "change",
    [
        {"order": 6},
        {"decimation": 1},
        {"decimation": 513},
        {"offset": 1 << 25},
        {"scale": 0x19},
        {"hpf": 4},
        {"int_value": 1},
        {"int_div": 2},
        {"discard": 256},
        {"skip": 128},
    ],
)
def test_out_of_range_config_is_refused(change: dict[str, int]) -> None:
    with pytest.raises(ValueError):
        dataclasses.replace(
            ChainConfig(order=1, decimation=2), **change
        )  # pyright: ignore[reportArgumentType]
