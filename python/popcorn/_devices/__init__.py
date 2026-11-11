"""Devices on the I2C and SPI buses and the UARTs of the MCU, written in Python:
`popcorn.board.I2CTarget`, `popcorn.board.SpiTarget`, `popcorn.board.SpiController`,
`popcorn.board.I2CController` and `popcorn.board.UartDevice`, served by the native models."""

from popcorn._devices.attach import attach, refuse_shadowed
from popcorn._devices.base import (
    AudioClock,
    BusDevice,
    BusHold,
    I2CPhases,
    SpiTransfer,
    parked,
    serving,
)
from popcorn._devices.i2c_controller import I2CAnswer, I2CController
from popcorn._devices.i2c_target import I2CTarget
from popcorn._devices.naming import check_address, chip_select, cs_of, spi_mode
from popcorn._devices.pins import DeviceBoard
from popcorn._devices.spi_controller import SpiController
from popcorn._devices.spi_target import SpiTarget
from popcorn._devices.uart import LineFormat, UartDevice
from popcorn._devices.watches import PinWatches, Unwatch

__all__ = [
    "AudioClock",
    "BusDevice",
    "BusHold",
    "DeviceBoard",
    "I2CAnswer",
    "I2CController",
    "I2CPhases",
    "I2CTarget",
    "LineFormat",
    "PinWatches",
    "SpiController",
    "SpiTarget",
    "SpiTransfer",
    "UartDevice",
    "Unwatch",
    "attach",
    "check_address",
    "chip_select",
    "cs_of",
    "parked",
    "refuse_shadowed",
    "serving",
    "spi_mode",
]
