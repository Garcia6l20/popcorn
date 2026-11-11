"""The virtual USB host of a device controller: cable, enumeration, descriptors, control
requests, CDC ACM streams, HID functions and USB Audio 2.0 functions, in virtual time.

`Periph(host=UsbHostOptions())` on a USB device controller in the machine description, OTG_FS on
the STM32U5 or USB on the STM32H5, plugs a full-speed host into the controller, and
`machine.periph.OTG_FS.host` or `machine.periph.USB.host` is its `UsbHost`. Everything below
Python runs in model events: no Python call per transaction.

Example:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(1))
    cdc = host.cdc()
    await cdc.wait_ready(for_=ms(100))
    await cdc.write(b"abc")

Time model:
    - A SOF goes every 1 ms. The isochronous and interrupt transactions of a frame go at its
      start, before any control or bulk transaction.
    - Control and bulk transactions take one slot each, round robin, every slot starting after
      the bus time of the previous transaction. A control stage the device NAKs while its
      interrupt handler runs is retried within the frame.
    - When every control and bulk pipe with work NAKed in a row, the next pass starts 10 us
      after that NAK ends, as long as its transaction fits before the next SOF.
    - A newly opened isochronous pipe sends its first packet 2 frames later, as Linux does.
      An interrupt pipe is first polled in the next frame.
    - After each bus reset ends the host sends only SOFs for 10 ms, the USB 2.0 7.1.7.5 reset
      recovery, then its next transaction.
    - The same writes at the same virtual times give the same transactions at the same times.

Snapshots:
    - Data Python wrote and the device has not taken is model state. A snapshot keeps it and a
      restore puts it back as it was, whether the function is bound or not.
    - Data the device sent and Python has not read is dropped at restore.
    - The request log, `UsbHost.requests`, is outside snapshots. A restore keeps it, so slice
      it from the length it had at the start of a test.

Drift:
    The host models no feedback endpoint and the device has no second clock. The device's
    isochronous rate is the host's frame rate, 1 ms per frame, so a test cannot show the clock
    drift that a real host and device correct with feedback or adaptive clocking.
"""

from popcorn._usb.cdc import CdcLineCoding, CdcPacket, CdcStream
from popcorn._usb.descriptors import (
    UsbEndpoint,
    UsbHostOptions,
    UsbInterface,
    UsbRequest,
    UsbState,
    UsbTransferType,
    requests_of,
)
from popcorn._usb.hid import HidReport, HidStream
from popcorn._usb.host import UsbHost, UsbHosts, checked_usb
from popcorn._usb.transfer import UsbData
from popcorn._usb.uac import (
    UacFeatureUnit,
    UacFormat,
    UacFrame,
    UacFunction,
    UacMic,
    UacSpeaker,
    UacStream,
)

__all__ = [
    "CdcLineCoding",
    "CdcPacket",
    "CdcStream",
    "HidReport",
    "HidStream",
    "UacFeatureUnit",
    "UacFormat",
    "UacFrame",
    "UacFunction",
    "UacMic",
    "UacSpeaker",
    "UacStream",
    "UsbData",
    "UsbEndpoint",
    "UsbHost",
    "UsbHostOptions",
    "UsbHosts",
    "UsbInterface",
    "UsbRequest",
    "UsbState",
    "UsbTransferType",
    "checked_usb",
    "requests_of",
]
