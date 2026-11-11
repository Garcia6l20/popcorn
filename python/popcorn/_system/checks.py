from __future__ import annotations

import asyncio
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, cast

from popcorn._devices import chip_select
from popcorn._errors import (
    ConfigError,
    MachineBusy,
)
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine
    from popcorn._streams import UartPorts
    from popcorn._system.links import LineState


def end_error(error: ConfigError, key: str) -> ConfigError:
    """`error`, the native refusal of a bus end, keyed by the end's description key `key`."""
    return ConfigError(
        f"{key}: {error.file} is not {error.expected}",
        file=error.file,
        key=key,
        expected=error.expected,
    )


def check_time(name: str, value: VirtualTime) -> None:
    if not isinstance(cast(object, value), VirtualTime):
        raise TypeError(f"{name} must be a VirtualTime, not {type(value).__name__}")


def line_low(line: LineState) -> bool:
    if not isinstance(cast(object, line), str):
        raise TypeError(f"line must be a str, not {type(line).__name__}")
    if line not in ("idle", "break"):
        raise ValueError(f"line must be 'idle' or 'break', not {line!r}")
    return line == "break"


def data_frames(data: bytes) -> list[int]:
    if not isinstance(cast(object, data), bytes | bytearray | memoryview):
        raise TypeError(f"data must be bytes, not {type(data).__name__}")
    frames = list(bytes(data))
    if not frames:
        raise ValueError("data must hold at least one byte")
    return frames


def check_count(name: str, value: int, least: int) -> None:
    if isinstance(cast(object, value), bool) or not isinstance(cast(object, value), int):
        raise TypeError(f"{name} must be an int, not {type(value).__name__}")
    if value < least:
        raise ValueError(f"{name} must be at least {least}, not {value}")


def unmark(marked: list[tuple[UartPorts, str]]) -> None:
    """Give a closed or collected system's UARTs back to their streams and ptys."""
    for ports, unit in marked:
        ports.linked.discard(unit)


def on_loop(loop: asyncio.AbstractEventLoop) -> bool:
    try:
        return asyncio.get_running_loop() is loop
    except RuntimeError:
        return False


def check_member(name: str, machine: Machine) -> None:
    if machine.name != name:
        raise ConfigError(
            f"members.{name}: the machine is named {machine.name!r}",
            key=f"members.{name}",
            expected=f"a Machine named {name!r}",
        )
    owner = machine._system  # pyright: ignore[reportPrivateUsage]
    if owner is not None and owner() is not None:
        raise ConfigError(
            f"members.{name}: the machine is a member of another linked system",
            key=f"members.{name}",
            expected="a machine in no linked system",
        )
    if machine.busy:
        raise MachineBusy(f"members.{name}: the machine is busy")


def check_target(for_: VirtualDuration | None, until: VirtualTime | None) -> None:
    if (for_ is None) == (until is None):
        raise TypeError("run takes for_ or until")
    if for_ is not None:
        if not isinstance(cast(object, for_), VirtualDuration):
            raise TypeError(f"for_ must be a VirtualDuration, not {type(for_).__name__}")
        if for_.ps < 0:
            raise ValueError("for_ must not be negative")
    elif not isinstance(cast(object, until), VirtualTime):
        raise TypeError(f"until must be a VirtualTime, not {type(until).__name__}")


def check_every(every: VirtualDuration, for_: VirtualDuration | None) -> None:
    for name, given in (("every", every), ("for_", for_)):
        if given is not None and not isinstance(cast(object, given), VirtualDuration):
            raise TypeError(f"{name} must be a VirtualDuration, not {type(given).__name__}")
    if every.ps <= 0:
        raise ValueError("every must be above 0")
    if for_ is not None and for_.ps < 0:
        raise ValueError("for_ must not be negative")


def offsets_of(names: Sequence[str], offsets: Mapping[str, VirtualDuration]) -> list[int]:
    for name, given in offsets.items():
        offset = cast(object, given)
        if name not in names:
            raise ConfigError(
                f"offsets.{name}: no such member", key=f"offsets.{name}", expected="a member name"
            )
        if not isinstance(offset, VirtualDuration) or offset.ps < 0:
            raise ConfigError(
                f"offsets.{name}: {offset!r} is no duration of at least 0",
                key=f"offsets.{name}",
                expected="a VirtualDuration of at least 0",
            )
    return [offsets[name].ps if name in offsets else 0 for name in names]


def checked_chip_select(owner: str, select: object) -> tuple[str, int | None] | None:
    """The GPIO port and pin of chip select `select`, `"<owner>.<pin>"` or `"<owner>.NSS"`
    for the SS output, None when it is neither."""
    if not isinstance(select, str):
        return None
    name, dot, pin = select.partition(".")
    if not dot or name != owner:
        return None
    if pin == "NSS":
        return "", None
    try:
        port, number = chip_select(pin)
    except ValueError:
        return None
    return port, number


def is_machine_name(name: str) -> bool:
    from popcorn._machine import check_machine_name

    try:
        check_machine_name(name)
    except ValueError:
        return False
    return True
