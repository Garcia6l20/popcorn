from __future__ import annotations

from dataclasses import dataclass
from typing import cast

_MASK64 = (1 << 64) - 1
_GAMMA = 0x9E37_79B9_7F4A_7C15
_MAX_COUNT = 1 << 20
MAX_QUEUED = 1 << 16
"""Most words an RNG word queue holds."""


def checked_int(name: str, value: object, top: int) -> int:
    """`value` as an int from 0 to `top`.

    Raises:
        TypeError: `value` is a bool or not an int, named as `name`.
        ValueError: `value` is outside 0 to `top`, named as `name`.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name}: expected an int, not {type(value).__qualname__}")
    if not 0 <= value <= top:
        raise ValueError(f"{name}: expected 0 to {top}, not {value}")
    return value


@dataclass(frozen=True, slots=True)
class RngOptions:
    """Word source of the built-in RNG model.

    Attributes:
        seed: Seed of the word generator, 0 to 2**64 - 1. `rng_words` gives the words it
            produces.

    Raises:
        TypeError: `seed` is a bool or not an int.
        ValueError: `seed` is outside 0 to 2**64 - 1.
    """

    seed: int = 0

    def __post_init__(self) -> None:
        checked_int("seed", cast(object, self.seed), _MASK64)


def _word_at(seed: int, position: int) -> int:
    z = (seed + (position + 1) * _GAMMA) & _MASK64
    z = ((z ^ (z >> 30)) * 0xBF58_476D_1CE4_E5B9) & _MASK64
    z = ((z ^ (z >> 27)) * 0x94D0_49BB_1331_11EB) & _MASK64
    z ^= z >> 31
    return z >> 32


def rng_words(seed: int, count: int, start: int = 0) -> list[int]:
    """The words the built-in RNG generator with `seed` produces from position `start`.

    Word `k` is the upper 32 bits of the SplitMix64 finalizer of
    `seed + (k + 1) * 0x9E3779B97F4A7C15`, modulo 2**64. A word of 0 is skipped, RM0456 reading
    a DR of 0 as a seed error, and still moves the position: `start` counts positions, as
    `RngEntropy.drawn` does.

    Raises:
        TypeError: An argument is a bool or not an int.
        ValueError: `seed` is outside 0 to 2**64 - 1, `count` outside 0 to 2**20 or `start`
            outside 0 to 2**64 - 1.
    """
    checked_int("seed", seed, _MASK64)
    checked_int("count", count, _MAX_COUNT)
    position = checked_int("start", start, _MASK64)
    words: list[int] = []
    while len(words) < count:
        word = _word_at(seed, position)
        position += 1
        if word != 0:
            words.append(word)
    return words
