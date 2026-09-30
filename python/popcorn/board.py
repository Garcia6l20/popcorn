"""What is wired to the MCU: UART streams, host ptys, GPIO pins, the NVIC, I2C and SPI devices."""

from __future__ import annotations

from popcorn._devices import I2CTarget, SpiTarget
from popcorn._enums import PinMode
from popcorn._nvic import Irqs, Nvic
from popcorn._streams import Board, Frame, UartStream
from popcorn._views import Pin

__all__ = [
    "Board",
    "Frame",
    "I2CTarget",
    "Irqs",
    "Nvic",
    "Pin",
    "PinMode",
    "SpiTarget",
    "UartStream",
]
