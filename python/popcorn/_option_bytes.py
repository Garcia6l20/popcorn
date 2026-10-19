from __future__ import annotations

from dataclasses import dataclass
from typing import Literal, cast

_ADDRESS_MAX = 0xFFFF_FFFF


@dataclass(frozen=True, slots=True, kw_only=True)
class OptionBytes:
    """User option bytes the machine boots with. The FLASH model of the part encodes them.

    The fields are those every STM32 family with TrustZone shares. Pages are numbered within
    their bank, first and last both included. `MachineDescription.machine` and `Machine` refuse
    what the part's family does not allow, such as a page past the bank, with a ConfigError keyed
    `option_bytes.<field>`.

    Attributes:
        tzen: Global TrustZone enable. The core then boots Secure from `secboot`.
        secwm1: Secure area of bank 1: a `(first, last)` page pair, "all" for the whole bank as
            after TZEN goes from 0 to 1, None for no secure area.
        secwm2: Secure area of bank 2, as `secwm1`.
        hdp1: Hide-protection area of bank 1, a `(first, last)` page pair starting at the first
            page of `secwm1`, None for none.
        hdp2: Hide-protection area of bank 2, as `hdp1` inside `secwm2`.
        secboot: Secure boot address, None for the family default, 0x0C000000 on the STM32U5.
        nsboot: Non-secure boot address, None for the family default, 0x08000000 on the STM32U5.
        swap_bank: Bank 1 and bank 2 addresses swapped.
        dualbank: Dual-bank organization, None for the family default.

    Raises:
        ValueError: A field has the wrong type, a page pair is not two pages from 0 to
            0xffffffff with the first not above the last, an HDP area is set without a secure
            area, or a boot address is not from 0 to 0xffffffff. The message starts with
            `option_bytes.<field>`.
    """

    tzen: bool = False
    secwm1: tuple[int, int] | Literal["all"] | None = "all"
    secwm2: tuple[int, int] | Literal["all"] | None = "all"
    hdp1: tuple[int, int] | None = None
    hdp2: tuple[int, int] | None = None
    secboot: int | None = None
    nsboot: int | None = None
    swap_bank: bool = False
    dualbank: bool | None = None

    def __post_init__(self) -> None:
        for name in ("tzen", "swap_bank"):
            _check_bool(name, getattr(self, name))
        if self.dualbank is not None:
            _check_bool("dualbank", self.dualbank)
        for name in ("secwm1", "secwm2"):
            value = cast(object, getattr(self, name))
            if value != "all" and value is not None:
                object.__setattr__(self, name, _pages(name, value, '"all", None or '))
        for name, area in (("hdp1", "secwm1"), ("hdp2", "secwm2")):
            value = cast(object, getattr(self, name))
            if value is None:
                continue
            object.__setattr__(self, name, _pages(name, value, "None or "))
            if getattr(self, area) is None:
                raise _error(name, f"no hide-protection area while {area} is None")
        for name in ("secboot", "nsboot"):
            value = cast(object, getattr(self, name))
            if value is None:
                continue
            if isinstance(value, bool) or not isinstance(value, int):
                raise _error(name, f"an address or None, not {type(value).__qualname__}")
            if not 0 <= value <= _ADDRESS_MAX:
                raise _error(name, f"an address from 0 to 0xffffffff, not {value:#x}")


def _error(name: str, expected: str) -> ValueError:
    return ValueError(f"option_bytes.{name}: expected {expected}")


def _check_bool(name: str, value: object) -> None:
    if not isinstance(value, bool):
        raise _error(name, f"a bool, not {type(value).__qualname__}")


def _pages(name: str, value: object, others: str) -> tuple[int, int]:
    expected = f"{others}a (first, last) pair of pages from 0 to 0xffffffff, first not above last"
    if not isinstance(value, tuple | list) or len(cast(tuple[object, ...], value)) != 2:
        raise _error(name, f"{expected}, not {value!r}")
    first, last = cast(tuple[object, object], tuple(cast(tuple[object, ...], value)))
    if (
        isinstance(first, bool)
        or isinstance(last, bool)
        or not isinstance(first, int)
        or not isinstance(last, int)
        or not 0 <= first <= last <= _ADDRESS_MAX
    ):
        raise _error(name, f"{expected}, not {value!r}")
    return (first, last)
