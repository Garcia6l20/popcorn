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
    SECURE = 6


class ResetKind(enum.Enum):
    """Cause of a reset. `PERIPHERAL` resets one peripheral through its bus reset bit, such as an
    STM32 RCC reset bit, and is what the default `Model.on_bus_reset()` passes to `reset()`.
    `Machine.reset()` refuses it."""

    POWER_ON = 0
    BROWN_OUT = 1
    PIN = 2
    SOFTWARE = 3
    WATCHDOG = 4
    WINDOW_WATCHDOG = 5
    LOW_POWER = 6
    OPTION_LOAD = 7
    PERIPHERAL = 8


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
    """An error an `I2CTarget` hook returns for the byte it answers, or an `I2CAnswer` carries,
    RM0456 65.4.17."""

    BUS_ERROR = 1
    """ISR.BERR: a misplaced START or STOP. The byte completes as if acknowledged."""
    ARBITRATION_LOST = 2
    """ISR.ARLO: another controller won the bus. The controller releases it and BUSY clears one
    STOP length later, at the other controller's STOP."""
    OVERRUN = 3
    """ISR.OVR: an overrun or underrun. Set on silicon only in target mode with NOSTRETCH, so
    this exercises firmware handlers. The byte completes as if acknowledged."""


class I2CPhase(enum.Enum):
    """The bus phase an `I2CController` answer is for."""

    ADDRESS = 0
    """A START or repeated START and its address byte."""
    WRITE = 1
    """A byte the controller writes."""
    READ = 2
    """A byte the controller reads."""


class Parity(enum.Enum):
    """The parity bit of a `LineFormat`, placed after the 8 data bits."""

    NONE = 0
    """No parity bit."""
    EVEN = 1
    """The data bits and the parity bit hold an even number of ones."""
    ODD = 2
    """The data bits and the parity bit hold an odd number of ones."""


class UartError(enum.Enum):
    """A line error. `UartStream.write` tags its bytes with FRAMING, NOISE or PARITY, RM0456 66.8
    USART_ISR: the flag rises when the byte reaches RDR, behind the bytes ahead of it in the RX
    FIFO. `UartDevice.on_error` hears ABORTED, and FRAMING or PARITY for a frame read in its own
    `LineFormat`."""

    FRAMING = 1
    """ISR.FE: the stop bit sampled 0. The byte arrives with its data."""
    NOISE = 2
    """ISR.NE: samples of a bit disagreed. The byte arrives with its data."""
    PARITY = 3
    """ISR.PE: the parity bit inverted. With CR1.PCE 0 the byte arrives clean, logged once."""
    ABORTED = 4
    """The firmware cleared UE with a frame in flight, on a UART whose UE discards it, or reset the
    UART through its RCC bit: the frame never completes. `UartStream.write` refuses it."""
