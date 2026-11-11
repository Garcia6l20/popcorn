from __future__ import annotations

import typing
import weakref

from popcorn._devices.base import ATTACHED, AudioClock, BusDevice, BusHold, I2CPhases, SpiTransfer
from popcorn._devices.i2c_controller import I2CController
from popcorn._devices.i2c_target import I2CTarget
from popcorn._devices.naming import private_names, reserved_message
from popcorn._devices.spi_controller import SpiController
from popcorn._devices.spi_target import SpiTarget
from popcorn._devices.uart import UartDevice

if typing.TYPE_CHECKING:
    pass


def refuse_shadowed(device: object) -> None:
    """Refuse a device of a Python device base that holds an instance attribute named like a
    private method of its base, since native code looks these up on the instance.

    Raises:
        TypeError: `device` sets such an attribute.
    """
    for base in (I2CTarget, SpiTarget, SpiController, I2CController, UartDevice):
        if not isinstance(device, base):
            continue
        taken = sorted(set(vars(device)) & private_names(base))
        if taken:
            raise TypeError(reserved_message(type(device).__qualname__, "sets", taken, base))


def attach(
    device: BusDevice,
    machine: object,
    audio_clock: AudioClock,
    bus_hold: BusHold,
    spi_transfer: SpiTransfer,
    i2c_phases: I2CPhases,
    peripheral: str,
) -> None:
    """Mark `device` as serving `machine` on `peripheral`, `audio_clock` a method of `machine`
    its `DeviceBoard` calls, `bus_hold` one `I2CTarget.hold_bus` calls, `spi_transfer` one
    `SpiController.transfer` calls, `i2c_phases` one the `I2CController` phase methods call. The
    mark is weak: once that machine is gone the device may serve another one. Keyed by
    identity, so a device defining `__eq__` works."""
    key = id(device)
    if key not in ATTACHED:
        weakref.finalize(device, ATTACHED.pop, key, None)
    ATTACHED[key] = (
        weakref.ref(machine),
        weakref.WeakMethod(audio_clock),
        weakref.WeakMethod(bus_hold),
        weakref.WeakMethod(spi_transfer),
        weakref.WeakMethod(i2c_phases),
        peripheral,
    )
