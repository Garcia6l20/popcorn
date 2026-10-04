"""What is wired to the MCU: UART streams, SAI audio streams, USB hosts, host ptys, GPIO pins,
the NVIC, I2C and SPI devices, and the clock sources of the RCC."""

from __future__ import annotations

from popcorn._audio import AudioFrame, AudioOptions, AudioStream
from popcorn._devices import I2CTarget, SpiTarget
from popcorn._enums import PinMode
from popcorn._nvic import Irqs, Nvic
from popcorn._rcc import RccOptions
from popcorn._streams import Board, Frame, UartStream
from popcorn._usb import (
    CdcLineCoding,
    CdcPacket,
    CdcStream,
    HidReport,
    HidStream,
    UacFeatureUnit,
    UacFormat,
    UacFrame,
    UacFunction,
    UacMic,
    UacSpeaker,
    UacStream,
    UsbEndpoint,
    UsbHost,
    UsbHostOptions,
    UsbInterface,
    UsbRequest,
    UsbState,
    UsbTransferType,
)
from popcorn._views import Pin

__all__ = [
    "AudioFrame",
    "AudioOptions",
    "AudioStream",
    "Board",
    "CdcLineCoding",
    "CdcPacket",
    "CdcStream",
    "Frame",
    "HidReport",
    "HidStream",
    "I2CTarget",
    "Irqs",
    "Nvic",
    "Pin",
    "PinMode",
    "RccOptions",
    "SpiTarget",
    "UacFeatureUnit",
    "UacFormat",
    "UacFrame",
    "UacFunction",
    "UacMic",
    "UacSpeaker",
    "UacStream",
    "UartStream",
    "UsbEndpoint",
    "UsbHost",
    "UsbHostOptions",
    "UsbInterface",
    "UsbRequest",
    "UsbState",
    "UsbTransferType",
]
