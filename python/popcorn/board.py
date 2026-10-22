"""What is wired to the MCU: UART streams, SAI audio streams, PDM microphones and digital filter
outputs, USB hosts, host ptys, GPIO pins, ADC inputs, the NVIC, I2C and SPI devices, SPI
controllers, the clock sources of the RCC, the supply voltages and the RNG word source."""

from __future__ import annotations

from popcorn._audio import AudioFrame, AudioLayout, AudioOptions, AudioStream
from popcorn._devices import DeviceBoard, I2CTarget, SpiController, SpiTarget
from popcorn._enums import I2CError, PinMode, UartError
from popcorn._nvic import Irqs, Nvic
from popcorn._pdm import FilterSample, FilterTap, PdmMic
from popcorn._rcc import RccOptions
from popcorn._rng import RngOptions, rng_words
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
from popcorn._views import AnalogInput, Pin, RngEntropy

__all__ = [
    "AnalogInput",
    "AudioFrame",
    "AudioLayout",
    "AudioOptions",
    "AudioStream",
    "Board",
    "CdcLineCoding",
    "CdcPacket",
    "CdcStream",
    "DeviceBoard",
    "FilterSample",
    "FilterTap",
    "Frame",
    "HidReport",
    "HidStream",
    "I2CError",
    "I2CTarget",
    "Irqs",
    "Nvic",
    "PdmMic",
    "Pin",
    "PinMode",
    "RccOptions",
    "RngEntropy",
    "RngOptions",
    "SpiController",
    "SpiTarget",
    "SupplyOptions",
    "UacFeatureUnit",
    "UacFormat",
    "UacFrame",
    "UacFunction",
    "UacMic",
    "UacSpeaker",
    "UacStream",
    "UartError",
    "UartStream",
    "UsbData",
    "UsbEndpoint",
    "UsbHost",
    "UsbHostOptions",
    "UsbInterface",
    "UsbRequest",
    "UsbState",
    "UsbTransferType",
    "rng_words",
]
