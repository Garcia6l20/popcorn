from __future__ import annotations

import difflib
import functools
import weakref
from collections.abc import Callable, Mapping, Sequence
from typing import (
    TYPE_CHECKING,
    Any,
    cast,
)

from popcorn import _core
from popcorn._audio import AudioOptions, Block
from popcorn._devices import (
    BusDevice,
    I2CController,
    I2CTarget,
    SpiController,
    SpiTarget,
    UartDevice,
    cs_of,
    refuse_shadowed,
    serving,
)
from popcorn._errors import (
    ConfigError,
)
from popcorn._machine_events import DEFAULT_ARENA_BASE
from popcorn._model_build import build_model
from popcorn._option_bytes import OptionBytes
from popcorn._part_description import PartDescription
from popcorn._stop import Stop, stop_of
from popcorn._streams import (
    PtyOpener,
)
from popcorn.devices import (
    At25sf,
    Eeprom24xx,
    I2CControllerBase,
    I2CTargetBase,
    SpiControllerBase,
    SpiTargetBase,
    TableTarget,
)
from popcorn.model import Model
from popcorn.plugin._records import Loaded

if TYPE_CHECKING:
    from popcorn._machine import Machine


def masked_stop(raw: object) -> tuple[Stop, int]:
    """The stop and the member mask a native conditions wait resolves to."""
    info, mask = cast(tuple[_core.StopInfo, int], raw)
    return stop_of(info), mask


def uart_attacher(
    handle: _core.MachineHandle, attached: Mapping[str, Sequence[BusDevice]]
) -> Callable[[str], _core.UartQueue]:
    """Connect a UART of `handle` to Python, refusing a UART whose far end is a device. Holds the
    handle and not the `Machine`."""
    devices = {
        name for name, targets in attached.items() for t in targets if isinstance(t, UartDevice)
    }

    def connected(name: str) -> _core.UartQueue:
        if name in devices:
            raise ConfigError(
                f"{name} has a UartDevice as its far end: no stream, pty or link next to it",
                key=f"devices.{name}",
                expected="a UART with no UartDevice",
            )
        return handle.attach_uart(name)

    return connected


def pty_opener(handle: _core.MachineHandle) -> PtyOpener:
    """Open pty bridges on `handle`, holding the handle and not the `Machine`."""

    def opened(name: str, link: _core.UartQueue | _core.CdcLink) -> _core.PtyBridge:
        if isinstance(link, _core.CdcLink):
            return handle.open_cdc_pty(name, link)
        return handle.open_uart_pty(name, link)

    return opened


def option_bytes_config(option_bytes: OptionBytes) -> _core.OptionBytesConfig:
    if not isinstance(cast(object, option_bytes), OptionBytes):
        raise TypeError(
            f"option_bytes: expected an OptionBytes, not {type(option_bytes).__qualname__}"
        )
    config = _core.OptionBytesConfig()
    config.tzen = option_bytes.tzen
    config.secwm1_all = option_bytes.secwm1 == "all"
    config.secwm1 = option_bytes.secwm1 if isinstance(option_bytes.secwm1, tuple) else None
    config.secwm2_all = option_bytes.secwm2 == "all"
    config.secwm2 = option_bytes.secwm2 if isinstance(option_bytes.secwm2, tuple) else None
    config.hdp1 = option_bytes.hdp1
    config.hdp2 = option_bytes.hdp2
    config.secboot = option_bytes.secboot
    config.nsboot = option_bytes.nsboot
    config.swap_bank = option_bytes.swap_bank
    config.dualbank = option_bytes.dualbank
    return config


def submitted(machine: weakref.ReferenceType[Machine], op: Any, at_ps: int) -> Any:
    held = machine()
    if held is None:
        raise RuntimeError("the loop's machine was collected")
    ops = [] if op is None else [op]
    handle = held._handle  # pyright: ignore[reportPrivateUsage]
    return handle.submit_ops(ops, [""] * len(ops), at_ps)


def model_makers(
    description: PartDescription, models: Mapping[str, Callable[[], Model]]
) -> dict[str, object]:
    served = {p.name for p in description.peripherals if p.alias_of is None}
    for name in models:
        if name not in served:
            close = difflib.get_close_matches(name, sorted(served))
            raise ConfigError(
                f"models: {description.part} has no peripheral {name}"
                + (f", close matches: {', '.join(close)}" if close else ""),
                file=description.part,
                key=f"models.{name}",
                expected="a peripheral of the description",
            )
    return {name: functools.partial(build_model, maker) for name, maker in models.items()}


def chosen_plugin_models(
    description: PartDescription,
    models: Mapping[str, object],
    plugin_models: Mapping[str, tuple[Loaded, str]],
) -> dict[str, tuple[Loaded, str]]:
    served = {p.name for p in description.peripherals if p.alias_of is None}
    for name in plugin_models:
        if name not in served or name in models:
            expected = (
                "a peripheral of the description" if name not in served else "one board model"
            )
            raise ConfigError(
                f"_plugin_models.{name}: {expected}",
                file=description.part,
                key=f"_plugin_models.{name}",
                expected=expected,
            )
    return dict(plugin_models)


FAULT_CONTEXT = (
    *(f"r{n}" for n in range(13)),
    "sp",
    "lr",
    "pc",
    "xpsr",
    "msp",
    "psp",
    "primask",
    "basepri",
    "faultmask",
    "control",
)

type AttachedDevice = (
    I2CTarget
    | SpiTarget
    | SpiController
    | I2CController
    | UartDevice
    | At25sf
    | Eeprom24xx
    | TableTarget
    | I2CTargetBase
    | SpiTargetBase
    | SpiControllerBase
    | I2CControllerBase
)
"""A device `Machine(devices=)` takes, of `popcorn.board`, of `popcorn.devices`, or of a native
plugin, derived from the bases the native bus models serve."""

_BUS_IPS: dict[type, tuple[str, ...]] = {
    I2CTarget: ("I2C",),
    SpiTarget: ("SPI",),
    SpiController: ("SPI",),
    I2CController: ("I2C",),
    UartDevice: ("USART", "UART", "LPUART"),
}


_CONTROLLERS: tuple[tuple[type, str], ...] = (
    (_core.SpiControllerBase, "SpiController"),
    (_core.I2CControllerBase, "I2CController"),
    (_core.UartDeviceBase, "UartDevice"),
)


def _device_kind(
    device: BusDevice,
) -> (
    type[I2CTarget] | type[SpiTarget] | type[SpiController] | type[I2CController] | type[UartDevice]
):
    if isinstance(device, _core.I2CTargetBase):
        return I2CTarget
    if isinstance(device, _core.UartDeviceBase):
        return UartDevice
    if isinstance(device, _core.SpiControllerBase):
        return SpiController
    if isinstance(device, _core.I2CControllerBase):
        return I2CController
    return SpiTarget


def _is_u32(value: object, lowest: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and lowest <= value < 1 << 32


def checked_audio(
    audio: Mapping[str, Mapping[Block, AudioOptions]],
    sais: Sequence[str],
    aliases: Mapping[str, str],
) -> dict[str, dict[Block, AudioOptions]]:
    """`audio` keyed by the SAI serving each key, checked against the built-in SAIs `sais`."""
    checked: dict[str, dict[Block, AudioOptions]] = {}
    for key, blocks in audio.items():
        name = aliases.get(key, key)
        if name not in sais:
            raise ConfigError(
                f"audio.{key}: an SAI with the built-in model, one of {', '.join(sais) or 'none'}",
                key=f"audio.{key}",
                expected="an SAI with the built-in model",
            )
        if name in checked:
            raise ConfigError(
                f"audio.{key}: {name} is listed twice, once through an alias",
                key=f"audio.{key}",
                expected="each SAI once",
            )
        checked[name] = {}
        for block, options in blocks.items():
            at = f"audio.{key}.{block}"
            if block not in ("A", "B"):
                raise ConfigError(f"{at}: an audio block A or B", key=at, expected="A or B")
            for field, value, lowest in (
                ("decimation", options.decimation, 1),
                ("external_frame_hz", options.external_frame_hz, 0),
            ):
                if not _is_u32(value, lowest):
                    raise ConfigError(
                        f"{at}.{field}: an int from {lowest} to 2**32 - 1, not {value!r}",
                        key=f"{at}.{field}",
                        expected=f"an int from {lowest} to 2**32 - 1",
                    )
            checked[name][block] = options
    return checked


def clock_wiring(
    audio: Mapping[str, Mapping[Block, AudioOptions]],
    attached: Mapping[str, Sequence[BusDevice]],
    aliases: Mapping[str, str],
) -> dict[tuple[str, Block], BusDevice]:
    """The device each `clocked_by` of `audio` names among the `attached` devices, keyed by the
    bus serving them, by SAI and block. A bus alias, such as `SEC_I2C1`, names the bus serving
    it. Of two devices matching, the first attached, as on the bus."""
    wired: dict[tuple[str, Block], BusDevice] = {}
    for name, blocks in audio.items():
        for block, options in blocks.items():
            named = options.clocked_by
            if named is None:
                continue
            at = f"audio.{name}.{block}.clocked_by"
            found = _device_named(named, attached, aliases)
            if found is None:
                raise ConfigError(
                    f'{at}: a device of devices such as "I2C1:0x38" or "SPI1:PA4", '
                    f"not {named!r}",
                    key=at,
                    expected='a bus device attached to the machine, such as "I2C1:0x38"',
                )
            wired[name, block] = found
    return wired


def _device_named(
    named: object, attached: Mapping[str, Sequence[BusDevice]], aliases: Mapping[str, str]
) -> BusDevice | None:
    if not isinstance(named, str):
        return None
    bus, colon, where = named.partition(":")
    if not colon:
        return None
    for device in attached.get(aliases.get(bus, bus), ()):
        if isinstance(device, _core.SpiTargetBase):
            if (cs_of(device) or "SS") == where:
                return device
            continue
        try:
            address = int(where, 0)
        except ValueError:
            return None
        if getattr(device, "address", None) == address:
            return device
    return None


def checked_devices(
    description: PartDescription,
    devices: Mapping[str, Sequence[BusDevice]],
    models: Mapping[str, object],
    plugin_models: Mapping[str, object],
) -> dict[str, tuple[BusDevice, ...]]:
    peripherals = {p.name: p for p in description.peripherals if p.alias_of is None}
    seen: set[int] = set()
    out: dict[str, tuple[BusDevice, ...]] = {}

    def refuse(name: str, expected: str) -> ConfigError:
        return ConfigError(
            f"devices.{name}: {expected}",
            file=description.part,
            key=f"devices.{name}",
            expected=expected,
        )

    for name, targets in devices.items():
        peripheral = peripherals.get(name)
        if peripheral is None:
            raise refuse(name, f"a peripheral of {description.part}")
        if not any(peripheral.ip in ips for ips in _BUS_IPS.values()):
            raise refuse(name, "an I2C, SPI or UART peripheral")
        if name in models:
            raise refuse(name, "a peripheral without a Python model")
        if name in plugin_models:
            raise refuse(name, "a peripheral without a plugin model")
        listed: list[BusDevice] = []
        for candidate in cast(Sequence[object], targets):
            if not isinstance(
                candidate,
                _core.I2CTargetBase
                | _core.SpiTargetBase
                | _core.SpiControllerBase
                | _core.I2CControllerBase
                | _core.UartDeviceBase,
            ):
                raise refuse(
                    name,
                    "popcorn.board.I2CTarget, SpiTarget, SpiController, I2CController or"
                    " UartDevice or popcorn.devices instances",
                )
            target = candidate
            kind = _device_kind(target)
            listed.append(target)
            if peripheral.ip not in _BUS_IPS[kind]:
                if kind is UartDevice:
                    raise refuse(name, "a UART peripheral for a UartDevice")
                raise refuse(name, f"an {_BUS_IPS[kind][0]} peripheral for an {kind.__name__}")
            if id(target) in seen:
                raise refuse(name, f"{target!r} listed once")
            seen.add(id(target))
            if serving(target) is not None:
                raise refuse(name, f"{target!r} not serving another live machine")
            refuse_shadowed(target)
            cs = cs_of(target) if isinstance(target, _core.SpiTargetBase) else None
            if cs is not None:
                port = f"GPIO{cs[1]}"
                if port not in peripherals:
                    raise refuse(name, f"a chip select on a GPIO port of {description.part}")
        for base, named in _CONTROLLERS:
            if any(isinstance(d, base) for d in listed) and len(listed) > 1:
                raise refuse(name, f"one {named} alone on {name}, no other device next to it")
        if listed:
            out[name] = tuple(listed)
    return out


def checked_budget(budget: object) -> int | None:
    if budget is None:
        return None
    if isinstance(budget, bool) or not isinstance(budget, int):
        raise TypeError(f"the default budget is an int or None, not {type(budget).__name__}")
    if budget <= 0:
        raise ValueError(f"the default budget must be above 0, not {budget}")
    return budget


def free_sentinel(description: PartDescription) -> int:
    """First 16-byte-aligned address from the default arena base, else from 0x1000, below the
    Arm system region, that no region or peripheral covers: the return address of a call without
    an arena."""
    spans = [(r.start, r.start + r.size) for r in description.regions]
    spans += [(p.base, p.base + p.size) for p in description.peripherals]
    for start in (DEFAULT_ARENA_BASE, 0x1000):
        address = start
        while address < 0xE000_0000:
            covering = next((end for low, end in spans if low <= address < end), None)
            if covering is None:
                return address
            address = (covering + 15) & ~15
    raise ConfigError(
        "no free address below 0xE0000000 for a call return",
        file="machine",
        key="arena",
        expected="an arena or an unmapped address for call returns",
    )
