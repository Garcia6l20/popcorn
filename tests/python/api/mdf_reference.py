"""Pure Python reference of the MDF and ADF digital filter chain, from RM0456 39.4.8 and 39.8.

Written from the reference manual only. Every stage works on Python integers, which are exact.

Chain, Figure 335: symbol remap, high = +1 and low = -1, the delay line dropping the first
`skip` bits, the CIC computed by direct convolution with its impulse response and wrapped at
26 bits, the offset subtraction, SCALE, SAT at 24 bits, RSFLT, HPF, the discard of the first
`discard` samples, INT, then the 24-bit PCM written to the RXFIFO.

popcorn-defined, not silicon-exact, never to be cited as RM0456 behaviour:

- RSFLT: gain x3 then a 4th-order Butterworth low-pass of two direct form I biquads with Q2.28
  coefficients, cutoff `RSFLT_CUTOFF` x FRS, outputs saturated at 24 bits after the gain and
  after each biquad. The RM gives the cutoff 0.111 x FRS, a gain of about 9.5 dB and a global
  -3 dB point of 7061 Hz for CIC5 D 16 at 1.024 MHz, no coefficients. The cutoff is retuned so
  the global point lands on 7061 Hz, which moves the RSFLT's own -3 dB point above 0.111 x FRS.
  With decimation by 4 the output is the filtered sample of each group's last input.
- HPF: `y[n] = x[n] - x[n-1] + y[n-1] - (y[n-1] >> k)`, k 8, 7, 6, 4 for HPFC 0 to 3.
- Right shifts round toward minus infinity: SCALE, the biquads and INT.
- INT: the sum of `int_value` samples shifted right by log2 of `int_div`, kept to its low 24
  bits. The RM lists no saturation there, keeping the width is the application's job.
- `saturated[i]` is true when any stage saturated since the previous output sample, samples
  dropped by decimation, discard or INT included.
- Input before the first bit counts as 0, the state of a CIC whose taps are cleared.
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from operator import mul
from typing import Literal

Order = Literal["fastsinc", 1, 2, 3, 4, 5]

CIC_BITS = 26
PCM_BITS = 24
PCM_MAX = (1 << (PCM_BITS - 1)) - 1
PCM_MIN = -(1 << (PCM_BITS - 1))
COEFFICIENT_BITS = 28
RSFLT_GAIN = 3
RSFLT_CUTOFF = 0.1163
RSFLT_SECTIONS: tuple[tuple[int, int, int, int, int], ...] = (
    (21197333, 42394666, 21197333, -247306859, 63660735),
    (27296179, 54592358, 27296179, -318461391, 159210650),
)
HPF_SHIFTS = (8, 7, 6, 4)
INT_DIVISIONS = (128, 32, 4, 1)
MODULATOR_FULL_SCALE = 1 << 15
MODULATOR_I1_BOUND = 1 << 20
MODULATOR_I2_BOUND = 1 << 22


@dataclass(frozen=True, slots=True)
class ChainConfig:
    """One digital filter configuration.

    Attributes:
        order: CIC order, "fastsinc" or 1 to 5, CICMOD.
        decimation: CIC decimation ratio, MCICD + 1, 2 to 512.
        offset: OFFSET of OECxCR, 26-bit signed, subtracted from the CIC output.
        scale: SCALE code, Table 377.
        rsflt: Whether the RSFLT runs, RSFLTBYP 0.
        rsflt_decimate: Whether the RSFLT decimates by 4, RSFLTD 0.
        hpf: HPFC, None when HPFBYP is 1.
        int_value: INTVAL + 1, None for INTVAL 0, which bypasses INT and ignores INTDIV.
        int_div: Division of the INT output: 128, 32, 4 or 1, INTDIV.
        discard: NBDIS, samples dropped after the enable.
        skip: SKPDLY applied at the enable, bits dropped before the CIC.
    """

    order: Order
    decimation: int
    offset: int = 0
    scale: int = 0
    rsflt: bool = False
    rsflt_decimate: bool = True
    hpf: int | None = None
    int_value: int | None = None
    int_div: int = 128
    discard: int = 0
    skip: int = 0

    def __post_init__(self) -> None:
        if self.order not in ("fastsinc", 1, 2, 3, 4, 5):
            raise ValueError(f"order {self.order!r}")
        if not 2 <= self.decimation <= 512:
            raise ValueError(f"decimation {self.decimation}")
        if not -(1 << (CIC_BITS - 1)) <= self.offset < 1 << (CIC_BITS - 1):
            raise ValueError(f"offset {self.offset}")
        scale_factor(self.scale)
        if self.hpf is not None and not 0 <= self.hpf <= 3:
            raise ValueError(f"hpf {self.hpf}")
        if self.int_value is not None and not 2 <= self.int_value <= 128:
            raise ValueError(f"int_value {self.int_value}")
        if self.int_div not in INT_DIVISIONS:
            raise ValueError(f"int_div {self.int_div}")
        if not 0 <= self.discard <= 255:
            raise ValueError(f"discard {self.discard}")
        if not 0 <= self.skip <= 127:
            raise ValueError(f"skip {self.skip}")


@dataclass(frozen=True, slots=True)
class Output:
    """What the chain writes to the RXFIFO.

    Attributes:
        pcm: 24-bit signed samples.
        saturated: Whether SATF would set while producing each sample.
    """

    pcm: list[int]
    saturated: list[bool]


def wrap(value: int, bits: int) -> int:
    """@p value kept to its low @p bits bits, two's complement."""
    half = 1 << (bits - 1)
    return ((value + half) & ((1 << bits) - 1)) - half


def saturate(value: int) -> tuple[int, bool]:
    """@p value clamped to 24 bits signed, and whether it was clamped."""
    if value > PCM_MAX:
        return PCM_MAX, True
    if value < PCM_MIN:
        return PCM_MIN, True
    return value, False


def shift(value: int, amount: int) -> int:
    """@p value shifted left by @p amount, right with floor when negative."""
    return value << amount if amount >= 0 else value >> -amount


def sinc_response(order: int, decimation: int) -> list[int]:
    """Impulse response of SincN, the coefficients of `(1 + z^-1 + ... + z^-(D-1))^N`."""
    box = [1] * decimation
    response = [1]
    for _ in range(order):
        product = [0] * (len(response) + decimation - 1)
        for i, a in enumerate(response):
            for j, b in enumerate(box):
                product[i + j] += a * b
        response = product
    return response


def cic(samples: Sequence[int], order: Order, decimation: int) -> list[int]:
    """CIC output, unwrapped, by direct convolution with the impulse response.

    Output m is the response applied to the inputs ending at input `(m + 1) x D - 1`. FastSinc
    adds the Sinc2 output two decimated samples back, the `1 + z^-2D` factor.
    """
    if order == "fastsinc":
        sinc2 = cic(samples, 2, decimation)
        return [y + (sinc2[m - 2] if m >= 2 else 0) for m, y in enumerate(sinc2)]
    reversed_response = sinc_response(order, decimation)[::-1]
    length = len(reversed_response)
    out: list[int] = []
    for m in range(len(samples) // decimation):
        end = (m + 1) * decimation
        start = max(0, end - length)
        window = samples[start:end]
        out.append(sum(map(mul, reversed_response[length - len(window) :], window, strict=True)))
    return out


def scale_factor(code: int) -> tuple[int, int]:
    """SCALE code as `(mult, shift)`: the gain is `mult x 2^shift`, Table 377.

    Raises:
        ValueError: A reserved code.
    """
    if 0x00 <= code <= 0x18:
        step = code
        base = 0
    elif 0x20 <= code <= 0x2F:
        step = code - 0x20
        base = -8
    else:
        raise ValueError(f"SCALE {code:#04x} is reserved")
    if step % 2 == 0:
        return 1, base + step // 2
    return 3, base + (step - 1) // 2 - 1


def scale_db(code: int) -> float:
    """Gain of a SCALE code in dB."""
    mult, amount = scale_factor(code)
    return 20 * math.log10(mult * 2.0**amount)


def apply_scale(value: int, code: int) -> int:
    """@p value times the SCALE gain, right shifts with floor."""
    mult, amount = scale_factor(code)
    return shift(value * mult, amount)


def rsflt_design(cutoff: float) -> tuple[tuple[int, int, int, int, int], ...]:
    """Q2.28 `(b0, b1, b2, a1, a2)` of the two biquads of a Butterworth low-pass at @p cutoff
    x FRS, bilinear transform prewarped at the cutoff, low Q section first."""
    k = math.tan(math.pi * cutoff)
    unit = 1 << COEFFICIENT_BITS
    sections: list[tuple[int, int, int, int, int]] = []
    for pole in (1, 3):
        q = 1 / (2 * math.cos(pole * math.pi / 8))
        norm = 1 / (1 + k / q + k * k)
        b0 = round(k * k * norm * unit)
        a1 = round(2 * (k * k - 1) * norm * unit)
        a2 = round((1 - k / q + k * k) * norm * unit)
        sections.append((b0, 2 * b0, b0, a1, a2))
    return tuple(sections)


class Rsflt:
    """The popcorn RSFLT, one sample at a time."""

    def __init__(self) -> None:
        self._x = [[0, 0] for _ in RSFLT_SECTIONS]
        self._y = [[0, 0] for _ in RSFLT_SECTIONS]

    def step(self, value: int) -> tuple[int, bool]:
        """Filters one sample, returns it and whether a saturation occurred."""
        sample, flag = saturate(RSFLT_GAIN * value)
        for (b0, b1, b2, a1, a2), x, y in zip(RSFLT_SECTIONS, self._x, self._y, strict=True):
            total = b0 * sample + b1 * x[0] + b2 * x[1] - a1 * y[0] - a2 * y[1]
            out, clipped = saturate(total >> COEFFICIENT_BITS)
            flag |= clipped
            x[1], x[0] = x[0], sample
            y[1], y[0] = y[0], out
            sample = out
        return sample, flag


def rsflt_response(frequency: float, sample_rate: float) -> float:
    """Magnitude of the RSFLT at @p frequency, gain x3 excluded, from its integer coefficients."""
    angle = 2 * math.pi * frequency / sample_rate
    z = complex(math.cos(angle), -math.sin(angle))
    unit = 1 << COEFFICIENT_BITS
    total = complex(1, 0)
    for b0, b1, b2, a1, a2 in RSFLT_SECTIONS:
        total *= (b0 + b1 * z + b2 * z * z) / (unit + a1 * z + a2 * z * z)
    return abs(total)


class Hpf:
    """The popcorn HPF, one sample at a time."""

    def __init__(self, hpfc: int) -> None:
        self._k = HPF_SHIFTS[hpfc]
        self._x = 0
        self._y = 0

    def step(self, value: int) -> tuple[int, bool]:
        """Filters one sample, returns it and whether it saturated."""
        out, flag = saturate(value - self._x + self._y - (self._y >> self._k))
        self._x = value
        self._y = out
        return out, flag


def hpf_response(frequency: float, sample_rate: float, hpfc: int) -> float:
    """Magnitude of the HPF at @p frequency from its difference equation."""
    pole = 1 - 2.0 ** -HPF_SHIFTS[hpfc]
    angle = 2 * math.pi * frequency / sample_rate
    z = complex(math.cos(angle), -math.sin(angle))
    return abs((1 - z) / (1 - pole * z))


def _merge(samples: Iterable[tuple[int, bool]], group: int) -> list[tuple[list[int], bool]]:
    out: list[tuple[list[int], bool]] = []
    values: list[int] = []
    flag = False
    for value, saturated in samples:
        values.append(value)
        flag |= saturated
        if len(values) == group:
            out.append((values, flag))
            values = []
            flag = False
    return out


def run_chain(bits: Sequence[int], config: ChainConfig) -> Output:
    """Runs @p bits, 0 or 1 each, through the chain @p config describes."""
    samples = [1 if bit else -1 for bit in bits[config.skip :]]
    stage: list[tuple[int, bool]] = []
    for value in cic(samples, config.order, config.decimation):
        scaled = apply_scale(wrap(value, CIC_BITS) - config.offset, config.scale)
        stage.append(saturate(scaled))
    if config.rsflt:
        rsflt = Rsflt()
        filtered = [rsflt.step(value) for value, _ in stage]
        merged = [
            (value, flag or saturated)
            for (value, flag), (_, saturated) in zip(filtered, stage, strict=True)
        ]
        group = 4 if config.rsflt_decimate else 1
        stage = [(values[-1], flag) for values, flag in _merge(merged, group)]
    if config.hpf is not None:
        hpf = Hpf(config.hpf)
        stage = [
            (value, flag or saturated)
            for (value, flag), (_, saturated) in zip(
                (hpf.step(value) for value, _ in stage), stage, strict=True
            )
        ]
    if config.discard:
        pending = any(flag for _, flag in stage[: config.discard])
        stage = stage[config.discard :]
        if stage:
            stage[0] = (stage[0][0], stage[0][1] or pending)
    if config.int_value is not None:
        amount = -int(math.log2(config.int_div))
        stage = [
            (wrap(shift(sum(values), amount), PCM_BITS), flag)
            for values, flag in _merge(stage, config.int_value)
        ]
    return Output([value for value, _ in stage], [flag for _, flag in stage])


def modulate(pcm16: Sequence[int], bits_per_sample: Sequence[int]) -> list[int]:
    """Bitstream of a 2nd-order single-bit sigma-delta modulator, 1 for high.

    Each `pcm16[i]` is held for `bits_per_sample[i]` bits. Integer state in units of 2^-15 full
    scale, Boser-Wooley 0.5 and 0.5 scaled to integers, so no division appears:
    `v = i2 >= 0`, `fb = +-2^15`, `i1 += x - fb`, `i2 += i1 - 2 fb` with the old `i1`. The
    integrators are clamped to `MODULATOR_I1_BOUND` and `MODULATOR_I2_BOUND` so an overload
    stays bounded and deterministic. Starts from zero state.
    """
    if len(pcm16) != len(bits_per_sample):
        raise ValueError("one bit count per sample")
    i1 = 0
    i2 = 0
    bits: list[int] = []
    for x, count in zip(pcm16, bits_per_sample, strict=True):
        if not -MODULATOR_FULL_SCALE <= x < MODULATOR_FULL_SCALE:
            raise ValueError(f"sample {x}")
        for _ in range(count):
            bit = 1 if i2 >= 0 else 0
            fb = MODULATOR_FULL_SCALE if bit else -MODULATOR_FULL_SCALE
            i2 = min(max(i2 + i1 - 2 * fb, -MODULATOR_I2_BOUND), MODULATOR_I2_BOUND)
            i1 = min(max(i1 + x - fb, -MODULATOR_I1_BOUND), MODULATOR_I1_BOUND)
            bits.append(bit)
    return bits
