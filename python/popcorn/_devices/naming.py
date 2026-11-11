from __future__ import annotations

import annotationlib
import re
import typing
from typing import cast

from popcorn import _core

if typing.TYPE_CHECKING:
    pass

_CS = re.compile(r"P([A-Z])(\d{1,2})")


def pin_of(pin: str) -> tuple[str, int]:
    """The GPIO port and pin number of the package pin `pin`, such as ("GPIOA", 5) for "PA5".

    Raises:
        TypeError: `pin` is no str.
        ValueError: `pin` is no pin name such as "PA5", pin 0 to 15.
    """
    if not isinstance(cast(object, pin), str):
        raise TypeError(f"a pin is named as a str such as 'PA5', not {pin!r}")
    matched = _CS.fullmatch(pin)
    if matched is None or int(matched[2]) > 15:
        raise ValueError(f"a pin is named such as 'PA5', pin 0 to 15, not {pin!r}")
    return f"GPIO{matched[1]}", int(matched[2])


def chip_select(cs: str | None) -> tuple[str, int | None]:
    """The GPIO port and pin of chip select `cs`, ("", None) for the hardware SS output.

    Raises:
        ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15.
    """
    if cs is None:
        return "", None
    matched = _CS.fullmatch(cs)
    if matched is None or int(matched[2]) > 15:
        raise ValueError(f'a chip select is a pin name such as "PA4", not {cs!r}')
    return f"GPIO{matched[1]}", int(matched[2])


def spi_mode(owner: str, mode: int | None) -> int | None:
    """`mode` checked as an SPI mode of `owner`, CPOL << 1 | CPHA, None for no check.

    Raises:
        TypeError: `mode` is no int or None.
        ValueError: `mode` is outside 0 to 3.
    """
    if mode is None:
        return None
    if isinstance(mode, bool) or not isinstance(cast(object, mode), int):
        raise TypeError(f"{owner} takes mode as an int or None, not {mode!r}")
    if not 0 <= mode <= 3:
        raise ValueError(f"{owner} takes mode 0 to 3, not {mode}")
    return mode


def cs_of(device: _core.SpiTargetBase) -> str | None:
    """The chip select pin name of an SPI device, such as "PB0", None for the hardware SS output.

    Read from `_cs_port` and `_cs_pin`, the attributes the machine attaches the device by, and
    spelled as the SPI model names the pin.
    """
    port: object = getattr(device, "_cs_port", "")
    pin: object = getattr(device, "_cs_pin", None)
    if not isinstance(port, str) or not port or not isinstance(pin, int):
        return None
    return f"P{port[-1]}{pin}"


def check_address(address: int) -> None:
    """Refuse `address` unless it is a 7-bit I2C target address, 0x08 to 0x77.

    Raises:
        ValueError: It is not.
    """
    if isinstance(address, bool) or not 0x08 <= address <= 0x77:
        raise ValueError(f"an I2C target address is 0x08 to 0x77, not {address!r}")


def _own_names(cls: type) -> set[str]:
    annotations = annotationlib.get_annotations(cls, format=annotationlib.Format.FORWARDREF)
    return set(vars(cls)) | set(annotations)


def private_names(cls: type) -> frozenset[str]:
    return frozenset(n for n in _own_names(cls) if n.startswith("_") and not n.endswith("__"))


def reserved_message(owner: str, verb: str, taken: list[str], base: type) -> str:
    names = ", ".join(repr(name) for name in taken)
    many = len(taken) > 1
    return (
        f"{owner} {verb} {names}, private name{'s' if many else ''} of {base.__qualname__} "
        f"that popcorn reserves: rename {'them' if many else 'it'}"
    )


def refuse_reserved(cls: type, base: type, attributes: frozenset[str] = frozenset()) -> None:
    """Refuse a subclass `cls` of the device base `base` that defines a name the base reserves.

    The reserved names are the private names the base class defines, which the native bus model
    calls by name, and `attributes`, the private instance attributes native code reads by name.

    Raises:
        TypeError: `cls` defines a reserved name in its own body.
    """
    taken = sorted((private_names(base) | attributes) & _own_names(cls))
    if taken:
        raise TypeError(reserved_message(cls.__qualname__, "defines", taken, base))
