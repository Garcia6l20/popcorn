"""Pads of the ADC bank B channels that have a pad of their own, by CubeMX IP version.

The STM32L1 Cat.3 to Cat.6 ADC, `aditf3_v1*` with `CR2.ADC_CFG`, has eleven bank B channels on
pads of their own, RM0038 Rev 18 Table 41: ADC_IN0b on PB2, 1b to 3b on PF11 to PF13, 6b and 7b
on PF14 and PF15, 8b to 12b on PG0 to PG4. The datasheet pin tables give the same pads, DS10002,
DS10764, DS8890, DS10262 and DS8576, where the CubeMX pin data leaves the `ADC_IN0b` signal off
PB2 of several packages, so a package bonds channel c of bank B exactly when it bonds its pad.
"""

from collections.abc import Mapping
from fnmatch import fnmatchcase

BANK_B_PADS: Mapping[str, Mapping[int, tuple[str, int]]] = {
    "aditf3_v1*": {
        0: ("B", 2),
        1: ("F", 11),
        2: ("F", 12),
        3: ("F", 13),
        6: ("F", 14),
        7: ("F", 15),
        8: ("G", 0),
        9: ("G", 1),
        10: ("G", 2),
        11: ("G", 3),
        12: ("G", 4),
    },
}
"""IP version glob -> bank B channel -> (GPIO port letter, pin) of its own pad."""


def bank_b_pads(ip_version: str) -> Mapping[int, tuple[str, int]]:
    """The own bank B pads of an ADC of CubeMX version @p ip_version, empty when it has none."""
    none: Mapping[int, tuple[str, int]] = {}
    return next((pads for glob, pads in BANK_B_PADS.items() if fnmatchcase(ip_version, glob)), none)


def bonded_bank_b(ip_version: str, bonded: Mapping[str, int]) -> int:
    """Bank B channels whose own pad @p bonded holds, bit c for ADC_INcb. @p bonded maps a port
    letter to the mask of its bonded pins, as bonded_pins gives it."""
    return sum(
        1 << channel
        for channel, (port, pin) in bank_b_pads(ip_version).items()
        if bonded.get(port, 0) >> pin & 1
    )
