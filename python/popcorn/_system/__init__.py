"""Several machines on one virtual clock, their UARTs, SPIs and I2Cs linked machine to machine."""

from popcorn._system.description import SystemDescription, SystemSnapshot, SystemStop
from popcorn._system.links import I2CBus, LineState, SpiBus, UartLink
from popcorn._system.system import System
from popcorn._system.taps import I2CTap, I2CTapPhase, SpiTap, SpiTapFrame, UartTap

__all__ = [
    "I2CBus",
    "I2CTap",
    "I2CTapPhase",
    "LineState",
    "SpiBus",
    "SpiTap",
    "SpiTapFrame",
    "System",
    "SystemDescription",
    "SystemSnapshot",
    "SystemStop",
    "UartLink",
    "UartTap",
]
