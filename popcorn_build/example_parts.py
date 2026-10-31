"""Which parts each shipped example runs on, and the class decoding behind that choice.

The gate runs every example on its `conftest.py` part, then once more on each extra part of
`EXAMPLE_PARTS` through `--popcorn-part`. Together the runs put every class of the shipped U5 and
H5 parts, and of the STM32L1 parts of `L1_GROUP_A`, under at least one example. The other shipped
STM32L1 names, every order code of the line, carry attach and reach evidence only. An extra run
moves every machine of the example onto its part, so each of the example's ELFs fits each extra
part. `python -m popcorn_build.example_parts --list` prints one `examples/<chip>/<name> <part>`
line per extra run.
"""

from __future__ import annotations

import sys
from collections.abc import Mapping
from dataclasses import dataclass
from functools import cache
from typing import Final, NamedTuple


@dataclass(frozen=True, slots=True)
class ExampleParts:
    """The parts one example runs on.

    Attributes:
        conftest: The part its `conftest.py` names.
        extra: The parts its ELFs fit that it also runs on, on any die of the family. A U5 or
            H5 example runs on one part per class no other run covers. An STM32L1 example runs
            on one part per other category its builds fit, since the models differ by category:
            the xe build on Cat.3 of both dies, the xb build also on Cat.2.
        pins: The GPIO pins its tests and firmware use, such as `PA5`, bonded on every part.
        adc_channels: The external channels of ADC1, the single `ADC` on the STM32L1, that it
            uses, in `bonded_channels` of every part.
        flash_kib: The flash size its firmware requires, None for any.
        also: The parts of the other descriptions its `conftest.py` names, each with an ELF of
            its own, such as the Cat.1 build of an STM32L1 example.
    """

    conftest: str
    extra: tuple[str, ...]
    pins: tuple[str, ...]
    adc_channels: tuple[int, ...]
    flash_kib: int | None
    also: tuple[str, ...] = ()


class PartClass(NamedTuple):
    """What a firmware example can tell parts of one die apart by.

    Attributes:
        die: `U575/U585`, `H56x/H573`, or the DEV_ID of an STM32L1 part such as `DIE437`.
        flash_kib: The flash size, such as 1024 or 2048.
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
_DESCRIBED_FAMILY: Final = "L1"
_DESCRIBED_SUFFIXES: Final = ("x", "xA", "xX")
FLASH_REGIONS: Final = ("Flash", "Flash_Bank2")
DEV_ID_MASK: Final = 0xFFF
_PACKAGES: Final = {
    ("A", "I"): "UFBGA169",
    ("C", "T"): "LQFP48",
    ("C", "U"): "UFQFPN48",
    ("I", "K"): "UFBGA176",
    ("I", "T"): "LQFP176",
    ("L", "H"): "TFBGA225",
    ("M", "Y"): "WLCSP80",
    ("O", "Y"): "WLCSP90",
    ("Q", "H"): "UFBGA132",
    ("Q", "I"): "UFBGA132",
    ("R", "H"): "TFBGA64",
    ("R", "T"): "LQFP64",
    ("R", "V"): "VFQFPN68",
    ("R", "Y"): "WLCSP64",
    ("U", "Y"): "WLCSP63",
    ("V", "H"): "UFBGA100",
    ("V", "T"): "LQFP100",
    ("V", "Y"): "WLCSP104",
    ("Z", "T"): "LQFP144",
}


@cache
def _described_class(part: str, package: str) -> PartClass:
    from popcorn.stm32 import mcu

    description = mcu(part)
    flash = sum(region.size for region in description.regions if region.name in FLASH_REGIONS)
    dbgmcu = next(unit for unit in description.peripherals if unit.name == "DBGMCU")
    idcode = next(register for register in dbgmcu.registers if register.name.endswith("IDCODE"))
    return PartClass(f"DIE{idcode.reset & DEV_ID_MASK:03X}", flash // 1024, package, False)


def part_class(part: str) -> PartClass:
    """The class of a shipped part by its ordering code, `STM32<line><pins><flash><package>x[Q]`.

    The pin-count letter and the package letter are read together: `R` with `T` is LQFP64 and
    `R` with `V` is VFQFPN68, `M` with `Y` is WLCSP80 and `O` with `Y` is WLCSP90. On the U5 and
    H5 the die and flash size come from the name letters. On the STM32L1 the die is the DEV_ID of
    the shipped description's DBGMCU_IDCODE reset value and the flash size its `Flash` and
    `Flash_Bank2` regions, since one die carries several flash letters and categories. The
    decoding only groups parts, no model reads it.

    Raises:
        ValueError: `part` is not an ordering code of the shipped lines. An STM32L1 name raises
            what `popcorn.stm32.mcu` raises for it.
    """
    line, pins, flash, package = part[5:9], part[9:10], part[10:11], part[11:12]
    name = _PACKAGES.get((pins, package))
    described = line.startswith(_DESCRIBED_FAMILY) and part[12:] in _DESCRIBED_SUFFIXES
    if described and part.startswith("STM32") and name is not None:
        return _described_class(part, name)
    die = _DIES.get(line)
    size = _FLASH_KIB.get(flash)
    if (
        not part.startswith("STM32")
        or die is None
        or size is None
        or name is None
        or part[12:] not in ("x", "xQ")
    ):
        raise ValueError(f"{part!r} is not an ordering code of a shipped part")
    return PartClass(die, size, name, part.endswith("Q"))


L1_GROUP_A: Final = (
    "STM32L152RETx",
    "STM32L152RCTxA",
    "STM32L152RCTx",
    "STM32L152RBTxA",
    "STM32L152RBTx",
)
"""The STM32L1 parts the examples cover, one LQFP64 part per die."""

_L1_XE_EXTRA: Final = ("STM32L152RCTxA", "STM32L152RCTx")
_L1_XB_EXTRA: Final = (*_L1_XE_EXTRA, "STM32L152RBTxA")

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
    "stm32l152/adc": ExampleParts(
        "STM32L152RETx", _L1_XE_EXTRA, pins=("PA0",), adc_channels=(0,), flash_kib=None
    ),
    "stm32l152/crc": ExampleParts(
        "STM32L152RETx", _L1_XE_EXTRA, pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32l152/gpio": ExampleParts(
        "STM32L152RETx",
        _L1_XB_EXTRA,
        pins=("PA5", "PC13"),
        adc_channels=(),
        flash_kib=None,
        also=("STM32L152RBTx",),
    ),
    "stm32l152/i2c": ExampleParts(
        "STM32L152RETx",
        _L1_XE_EXTRA,
        pins=("PA5", "PB8", "PB9"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32l152/iwdg": ExampleParts(
        "STM32L152RETx", _L1_XE_EXTRA, pins=(), adc_channels=(), flash_kib=None
    ),
    "stm32l152/pwr_flash": ExampleParts(
        "STM32L152RETx",
        _L1_XB_EXTRA,
        pins=(),
        adc_channels=(),
        flash_kib=None,
        also=("STM32L152RBTx",),
    ),
    "stm32l152/rcc": ExampleParts(
        "STM32L152RETx",
        _L1_XB_EXTRA,
        pins=("PH0",),
        adc_channels=(),
        flash_kib=None,
        also=("STM32L152RBTx",),
    ),
    "stm32l152/rtc": ExampleParts(
        "STM32L152RETx", _L1_XE_EXTRA, pins=("PC14", "PC15"), adc_channels=(), flash_kib=None
    ),
    "stm32l152/spi": ExampleParts(
        "STM32L152RETx",
        _L1_XE_EXTRA,
        pins=("PA5", "PA6", "PA7", "PB6"),
        adc_channels=(),
        flash_kib=None,
    ),
    "stm32l152/tim": ExampleParts(
        "STM32L152RETx", _L1_XE_EXTRA, pins=("PA5", "PA6"), adc_channels=(), flash_kib=None
    ),
    "stm32l152/usart": ExampleParts(
        "STM32L152RETx",
        _L1_XB_EXTRA,
        pins=("PA2", "PA3"),
        adc_channels=(),
        flash_kib=None,
        also=("STM32L152RBTx",),
    ),
    "stm32l152/usb_cdc": ExampleParts(
        "STM32L152RETx",
        _L1_XE_EXTRA,
        pins=("PA11", "PA12", "PH0"),
        adc_channels=(),
        flash_kib=None,
    ),
}


TESTED_PARTS: Final = frozenset(
    part for row in EXAMPLE_PARTS.values() for part in (row.conftest, *row.also)
)
"""The parts the example firmware suites run on: the `conftest.py` part of every example and its
`also` parts."""

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
