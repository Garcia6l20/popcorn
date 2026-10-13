"""Enumerations shared with the native core, pinned to it by value."""

from __future__ import annotations

import enum


class StopReason(enum.Enum):
    """Why a run stopped."""

    RETURNED = 0
    ADDRESS = 1
    BUDGET = 2
    TIME = 3
    IDLE = 4
    FAULT = 5
    RESET_REQUEST = 6
    SPIN = 7
    WATCHPOINT = 8
    USER = 9
    CONTRACT = 10


class FaultKind(enum.Enum):
    """Class of a processor fault."""

    HARD = 0
    MEM_MANAGE = 1
    BUS = 2
    USAGE = 3
    UNMAPPED = 4
    INVALID = 5


class ResetKind(enum.Enum):
    """Cause of a reset."""

    POWER_ON = 0
    BROWN_OUT = 1
    PIN = 2
    SOFTWARE = 3
    WATCHDOG = 4
    WINDOW_WATCHDOG = 5
    LOW_POWER = 6
    OPTION_LOAD = 7


class FloatAbi(enum.Enum):
    """Floating-point calling convention of an image."""

    SOFT = 0
    HARD_SINGLE = 1
    HARD_DOUBLE = 2


class ValueKind(enum.Enum):
    """Kind of a call argument or result: its width and whether it is a float."""

    I32 = 0
    I64 = 1
    F32 = 2
    F64 = 3


class PinMode(enum.Enum):
    """Mode of a GPIO pin."""

    INPUT = 0
    OUTPUT = 1
    ALTERNATE = 2
    ANALOG = 3


class I2CError(enum.Enum):
    """An error an `I2CTarget` hook returns for the byte it answers, RM0456 65.4.17."""

    BUS_ERROR = 1
    """ISR.BERR: a misplaced START or STOP. The byte completes as if acknowledged."""
    ARBITRATION_LOST = 2
    """ISR.ARLO: another controller won the bus. The controller releases it and BUSY clears one
    STOP length later, at the other controller's STOP."""
    OVERRUN = 3
    """ISR.OVR: an overrun or underrun. Set on silicon only in target mode with NOSTRETCH, so
    this exercises firmware handlers. The byte completes as if acknowledged."""
