"""What is wired to the MCU: UART streams, SAI audio streams, USB hosts, host ptys, GPIO pins,
ADC inputs, the NVIC, I2C and SPI devices, the clock sources of the RCC and the supply voltages."""

from __future__ import annotations

from popcorn._audio import AudioFrame, AudioOptions, AudioStream
from popcorn._devices import DeviceBoard, I2CTarget, SpiTarget
from popcorn._enums import PinMode
from popcorn._nvic import Irqs, Nvic
from popcorn._rcc import RccOptions
from popcorn._streams import Board, Frame, UartStream
from popcorn._supply import SupplyOptions
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
    UsbData,
    UsbEndpoint,
    UsbHost,
    UsbHostOptions,
    UsbInterface,
    UsbRequest,
    UsbState,
    UsbTransferType,
)
from popcorn._views import AnalogInput, Pin

__all__ = [
    "AnalogInput",
    "AudioFrame",
    "AudioOptions",
    "AudioStream",
    "Board",
    "CdcLineCoding",
    "CdcPacket",
    "CdcStream",
    "DeviceBoard",
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
    "SupplyOptions",
    "UacFeatureUnit",
    "UacFormat",
    "UacFrame",
    "UacFunction",
    "UacMic",
    "UacSpeaker",
    "UacStream",
    "UartStream",
    "UsbData",
    "UsbEndpoint",
    "UsbHost",
    "UsbHostOptions",
    "UsbInterface",
    "UsbRequest",
    "UsbState",
    "UsbTransferType",
]
