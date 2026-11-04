"""Pads of the ADC bank B channels that have a pad of their own, and of the bank A channels no
package of their die bonds, by CubeMX IP version.

The STM32L1 Cat.3 to Cat.6 ADC, `aditf3_v1*` with `CR2.ADC_CFG`, has eleven bank B channels on
pads of their own, RM0038 Rev 18 Table 41: ADC_IN0b on PB2, 1b to 3b on PF11 to PF13, 6b and 7b
on PF14 and PF15, 8b to 12b on PG0 to PG4. The datasheet pin tables give the same pads, DS10002,
DS10764, DS8890, DS10262 and DS8576, where the CubeMX pin data leaves the `ADC_IN0b` signal off
PB2 of several packages, so a package bonds channel c of bank B exactly when it bonds its pad.

The same table gives ADC_IN27 to ADC_IN31 on PF6 to PF10. No Cat.3 package bonds those pins, so
the pin list of a Cat.3 reference part names no pad for those channels of its ADC.
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


UNBONDED_PADS: Mapping[str, Mapping[int, tuple[str, int]]] = {
    "aditf3_v1*": {
        27: ("F", 6),
        28: ("F", 7),
        29: ("F", 8),
        30: ("F", 9),
        31: ("F", 10),
    },
}
"""IP version glob -> bank A channel -> (GPIO port letter, pin) of a pad that the pin list of a
reference part may lack, RM0038 Rev 18 Table 41."""


def _pads(
    table: Mapping[str, Mapping[int, tuple[str, int]]], ip_version: str
) -> Mapping[int, tuple[str, int]]:
    none: Mapping[int, tuple[str, int]] = {}
    return next((pads for glob, pads in table.items() if fnmatchcase(ip_version, glob)), none)


def unbonded_pads(ip_version: str) -> Mapping[int, tuple[str, int]]:
    """The bank A pads an ADC of CubeMX version @p ip_version has beyond its reference part's pin
    list, empty when it has none."""
    return _pads(UNBONDED_PADS, ip_version)


def bank_b_pads(ip_version: str) -> Mapping[int, tuple[str, int]]:
    """The own bank B pads of an ADC of CubeMX version @p ip_version, empty when it has none."""
    return _pads(BANK_B_PADS, ip_version)


def bonded_bank_b(ip_version: str, bonded: Mapping[str, int]) -> int:
    """Bank B channels whose own pad @p bonded holds, bit c for ADC_INcb. @p bonded maps a port
    letter to the mask of its bonded pins, as bonded_pins gives it."""
    return sum(
        1 << channel
        for channel, (port, pin) in bank_b_pads(ip_version).items()
        if bonded.get(port, 0) >> pin & 1
    )
