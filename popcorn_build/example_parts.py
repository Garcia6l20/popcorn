"""Which parts each shipped example runs on, and the class decoding behind that choice.

The gate runs every example on its `conftest.py` part, then once more on each extra part of
`EXAMPLE_PARTS` through `--popcorn-part`. Together the runs put every class of the shipped parts
under at least one example. `python -m popcorn_build.example_parts --list` prints one
`examples/<chip>/<name> <part>` line per extra run.
"""

from __future__ import annotations

import sys
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final, NamedTuple


@dataclass(frozen=True, slots=True)
class ExampleParts:
    """The parts one example runs on.

    Attributes:
        conftest: The part its `conftest.py` names.
        extra: The parts of the same die it also runs on, one per class no other run covers.
        pins: The GPIO pins its tests and firmware use, such as `PA5`, bonded on every part.
        adc_channels: The ADC1 external channels it uses, in `bonded_channels` of every part.
        flash_kib: The flash size its firmware requires, None for any.
    """

    conftest: str
    extra: tuple[str, ...]
    pins: tuple[str, ...]
    adc_channels: tuple[int, ...]
    flash_kib: int | None


class PartClass(NamedTuple):
    """What a firmware example can tell parts of one die apart by.

    Attributes:
        die: `U575/U585` or `H56x/H573`.
        flash_kib: The flash size, 1024 or 2048.
        package: The package name, such as `LQFP64`.
        smps: Whether the part has the SMPS supply, a trailing `Q` in its ordering code.
    """

    die: str
    flash_kib: int
    package: str
    smps: bool


_DIES: Final = {
    "U575": "U575/U585",
    "U585": "U575/U585",
    "H562": "H56x/H573",
    "H563": "H56x/H573",
    "H573": "H56x/H573",
}
_FLASH_KIB: Final = {"G": 1024, "I": 2048}
_PACKAGES: Final = {
    ("A", "I"): "UFBGA169",
    ("C", "T"): "LQFP48",
    ("C", "U"): "UFQFPN48",
    ("I", "K"): "UFBGA176",
    ("I", "T"): "LQFP176",
    ("L", "H"): "TFBGA225",
    ("M", "Y"): "WLCSP80",
    ("O", "Y"): "WLCSP90",
    ("Q", "I"): "UFBGA132",
    ("R", "T"): "LQFP64",
    ("R", "V"): "VFQFPN68",
    ("V", "T"): "LQFP100",
    ("Z", "T"): "LQFP144",
}


def part_class(part: str) -> PartClass:
    """The class of a shipped part by its ordering code, `STM32<line><pins><flash><package>x[Q]`.

    The pin-count letter and the package letter are read together: `R` with `T` is LQFP64 and
    `R` with `V` is VFQFPN68, `M` with `Y` is WLCSP80 and `O` with `Y` is WLCSP90. The decoding
    only groups parts, no model reads it.

    Raises:
        ValueError: `part` is not an ordering code of the five lines.
    """
    line, pins, flash, package = part[5:9], part[9:10], part[10:11], part[11:12]
    die = _DIES.get(line)
    size = _FLASH_KIB.get(flash)
    name = _PACKAGES.get((pins, package))
    if (
        not part.startswith("STM32")
        or die is None
        or size is None
        or name is None
        or part[12:] not in ("x", "xQ")
    ):
        raise ValueError(f"{part!r} is not an ordering code of a shipped part")
    return PartClass(die, size, name, part.endswith("Q"))


EXAMPLE_PARTS: Final[Mapping[str, ExampleParts]] = {
    "stm32u575/adc": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575AGIx", "STM32U585CITx"),
        pins=(),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/adc_watchdog": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575RITx",),
        pins=("PA0", "PA1", "PC0", "PC1"),
        adc_channels=(1, 2, 5, 6),
        flash_kib=None,
    ),
    "stm32u575/crc": ExampleParts(
        "STM32U575QGIx", ("STM32U585CITxQ",), pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32u575/gpdma": ExampleParts(
        "STM32U575QGIx", ("STM32U575CIUx",), pins=("PA9",), adc_channels=(), flash_kib=None
    ),
    "stm32u575/gpio": ExampleParts(
        "STM32U575QGIx", ("STM32U575CIUxQ",), pins=("PA5", "PC13"), adc_channels=(), flash_kib=None
    ),
    "stm32u575/i2c": ExampleParts(
        "STM32U575QGIx", ("STM32U575CGTxQ",), pins=("PB6", "PB7"), adc_channels=(), flash_kib=None
    ),
    "stm32u575/i2c_mock": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575CGUx",),
        pins=("PA5", "PB6", "PB7"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/iwdg": ExampleParts(
        "STM32U575QGIx", ("STM32U575CGUxQ",), pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32u575/iwdg_ewi": ExampleParts(
        "STM32U575QGIx", ("STM32U575RITxQ",), pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32u575/linked_uart": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575VITxQ",),
        pins=("PA0", "PA1", "PA9", "PA10", "PB10", "PB11"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/native_plugin": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575RGTx",),
        pins=("PA5", "PA6", "PA7", "PB0"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/otg_fs": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575RGTxQ", "STM32U575AGIxQ"),
        pins=("PA11", "PA12"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/pdm_mics": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575VITx",),
        pins=("PD3", "PD6", "PE9"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/pwr_flash": ExampleParts(
        "STM32U575QGIx", ("STM32U575CGTx",), pins=(), adc_channels=(), flash_kib=1024
    ),
    "stm32u575/python_model": ExampleParts(
        "STM32U575QGIx", ("STM32U575ZGTx",), pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32u575/rcc": ExampleParts(
        "STM32U575QGIx", ("STM32U575VGTx",), pins=("PH0",), adc_channels=(), flash_kib=None
    ),
    "stm32u575/rng": ExampleParts(
        "STM32U575QGIx", ("STM32U585ZITx",), pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32u575/sai": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575OIYxQ",),
        pins=("PE4", "PE5", "PE6"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/sai_codec": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575OGYxQ",),
        pins=("PE3", "PE4", "PE5", "PE6", "PB6", "PB7"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/spi": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575QGIxQ",),
        pins=("PA4", "PA5", "PA6", "PA7"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/spi_mock": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575QIIx",),
        pins=("PA5", "PA6", "PA7", "PB0"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32u575/tim": ExampleParts(
        "STM32U575QGIx", ("STM32U575QIIxQ",), pins=("PA5",), adc_channels=(), flash_kib=None
    ),
    "stm32u575/trustzone": ExampleParts(
        "STM32U575QGIx", ("STM32U585AIIx",), pins=("PA5",), adc_channels=(), flash_kib=None
    ),
    "stm32u575/uart_mock": ExampleParts(
        "STM32U575QGIx", ("STM32U575AIIxQ",), pins=("PA2", "PA3"), adc_channels=(), flash_kib=None
    ),
    "stm32u575/usart": ExampleParts(
        "STM32U575QGIx", ("STM32U575VGTxQ",), pins=("PA9", "PA10"), adc_channels=(), flash_kib=None
    ),
    "stm32u575/usb_audio": ExampleParts(
        "STM32U575QGIx", ("STM32U575ZGTxQ",), pins=("PA11", "PA12"), adc_channels=(), flash_kib=None
    ),
    "stm32u575/usb_cdc": ExampleParts(
        "STM32U575QGIx",
        ("STM32U575ZITxQ",),
        pins=("PA11", "PA12", "PC13"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32h573/gpdma": ExampleParts(
        "STM32H573AIIx",
        ("STM32H562ZGTx", "STM32H562AGIx"),
        pins=("PG1",),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32h573/pwr_flash": ExampleParts(
        "STM32H573AIIx",
        ("STM32H562RITx", "STM32H563RIVx"),
        pins=(),
        adc_channels=(),
        flash_kib=2048,
    ),
    "stm32h573/rcc": ExampleParts(
        "STM32H573AIIx",
        (
            "STM32H562RGTx",
            "STM32H563ZITx",
            "STM32H562IITx",
            "STM32H563LIHxQ",
            "STM32H563IIKx",
            "STM32H563MIYxQ",
        ),
        pins=(),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32h573/spi": ExampleParts(
        "STM32H573AIIx",
        ("STM32H562VGTx", "STM32H563VITx"),
        pins=("PE2", "PE4", "PE5", "PE6"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32h573/usart": ExampleParts(
        "STM32H573AIIx",
        ("STM32H562IGTx", "STM32H563IGKx"),
        pins=("PG0", "PG1"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32h573/usb_cdc": ExampleParts(
        "STM32H573AIIx",
        (
            "STM32H563RGVx",
            "STM32H573VITxQ",
            "STM32H573ZITxQ",
            "STM32H573IITxQ",
            "STM32H563AIIxQ",
            "STM32H573IIKxQ",
        ),
        pins=("PA11", "PA12"),
        adc_channels=(),
        flash_kib=None,
    ),
}


TESTED_PARTS: Final = frozenset(row.conftest for row in EXAMPLE_PARTS.values())
"""The parts that run the whole firmware suites: the `conftest.py` part of every example."""

GEOMETRY_PARTS: Final = frozenset({"STM32U575ZITxQ", "STM32H563ZGTx"})
"""The parts the FLASH geometry cases of `stm32-flash-geometry` run on, 2 MB on the U5 and 1 MB
on the H5."""


def extra_runs() -> list[tuple[str, str]]:
    """One `(examples/<chip>/<name>, part)` pair per extra run, in table order."""
    return [(f"examples/{name}", part) for name, row in EXAMPLE_PARTS.items() for part in row.extra]


if __name__ == "__main__":
    if sys.argv[1:] != ["--list"]:
        raise SystemExit("usage: python -m popcorn_build.example_parts --list")
    for path, part in extra_runs():
        print(path, part)
