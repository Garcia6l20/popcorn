from __future__ import annotations

import enum
from collections.abc import Iterable
from dataclasses import dataclass

from popcorn import _core
from popcorn.units import VirtualDuration, VirtualTime, ms, s

LANGUAGES = 0
DEBOUNCE = ms(100)
RESET = ms(10)
TIMEOUT = s(5)


class UsbState(enum.Enum):
    """Device state as the host sees it, USB 2.0 9.1.1.

    DETACHED is no pull-up seen, POWERED a pull-up seen before the first bus reset ends,
    DEFAULT after it at address 0, ADDRESS after SET_ADDRESS, CONFIGURED after
    SET_CONFIGURATION.
    """

    DETACHED = 0
    POWERED = 1
    DEFAULT = 2
    ADDRESS = 3
    CONFIGURED = 4


class UsbTransferType(enum.Enum):
    """Transfer type of an endpoint, bits 1:0 of its bmAttributes."""

    CONTROL = 0
    ISOCHRONOUS = 1
    BULK = 2
    INTERRUPT = 3


@dataclass(frozen=True, slots=True)
class UsbEndpoint:
    """An endpoint descriptor of an interface.

    Attributes:
        address: bEndpointAddress, the number in bits 3:0, IN when bit 7 is set.
        transfer_type: Transfer type.
        attributes: bmAttributes, synchronization and usage bits included.
        max_packet: wMaxPacketSize, bits 10:0.
        interval: bInterval.
    """

    address: int
    transfer_type: UsbTransferType
    attributes: int
    max_packet: int
    interval: int

    @property
    def number(self) -> int:
        """Endpoint number, 0 to 15."""
        return self.address & 0x0F

    @property
    def is_in(self) -> bool:
        """Whether the endpoint is IN, device to host."""
        return bool(self.address & 0x80)


@dataclass(frozen=True, slots=True)
class UsbInterface:
    """One alternate setting of an interface of the configuration the device runs.

    Attributes:
        number: bInterfaceNumber.
        alternate: bAlternateSetting.
        class_code: bInterfaceClass.
        subclass: bInterfaceSubClass.
        protocol: bInterfaceProtocol.
        string_index: iInterface, 0 for none.
        endpoints: Endpoints, in descriptor order.
    """

    number: int
    alternate: int
    class_code: int
    subclass: int
    protocol: int
    string_index: int
    endpoints: tuple[UsbEndpoint, ...]


def requests_of(records: Iterable[_core.UsbRequestRecord]) -> tuple[UsbRequest, ...]:
    """The `UsbRequest` of each native request record, in order."""
    return tuple(
        UsbRequest(
            at=VirtualTime(record.at_ps),
            address=record.address,
            request_type=record.request_type,
            request=record.request,
            value=record.value,
            index=record.index,
            length=record.length,
            status=None if record.status is None else record.status.name,
        )
        for record in records
    )


@dataclass(frozen=True, slots=True)
class UsbHostOptions:
    """The virtual host a board wires to a USB device controller.

    Attributes:
        attached: Whether the cable is plugged at power-on. A power-on reset plugs it back as
            this says.
        debounce: Connect debounce before the attach reset, USB 2.0 7.1.7.3.
        reset: SE0 time of a bus reset, above 0, USB 2.0 7.1.7.5.
        timeout: Time a control transfer may take after its SETUP, above 0. Past it the
            transfer ends with UsbTimeout, and an enumeration step stops the enumeration.
        dtr: DTR in the SET_CONTROL_LINE_STATE the host sends each CDC ACM function at
            configuration. The host never drops it on its own afterwards.
        rts: RTS in that SET_CONTROL_LINE_STATE.
    """

    attached: bool = True
    debounce: VirtualDuration = DEBOUNCE
    reset: VirtualDuration = RESET
    timeout: VirtualDuration = TIMEOUT
    dtr: bool = True
    rts: bool = True

    def native(self) -> _core.UsbHostConfig:
        """The options as the native machine configuration takes them."""
        config = _core.UsbHostConfig()
        config.attached = self.attached
        config.debounce_ns = self.debounce.ps // 1000
        config.reset_ns = self.reset.ps // 1000
        config.timeout_ns = self.timeout.ps // 1000
        config.dtr = self.dtr
        config.rts = self.rts
        return config


@dataclass(frozen=True, slots=True)
class UsbRequest:
    """A control request the host sent, from its request log.

    Attributes:
        at: Virtual time of its SETUP.
        address: Device address.
        request_type: bmRequestType.
        request: bRequest.
        value: wValue.
        index: wIndex.
        length: wLength.
        status: How it ended, such as `completed` or `stalled`, None while it runs.
    """

    at: VirtualTime
    address: int
    request_type: int
    request: int
    value: int
    index: int
    length: int
    status: str | None


def interface_of(record: _core.UsbInterfaceRecord) -> UsbInterface:
    return UsbInterface(
        number=record.number,
        alternate=record.alternate,
        class_code=record.class_code,
        subclass=record.subclass,
        protocol=record.protocol,
        string_index=record.string_index,
        endpoints=tuple(
            UsbEndpoint(
                address=endpoint.address,
                transfer_type=UsbTransferType(endpoint.type),
                attributes=endpoint.attributes,
                max_packet=endpoint.max_packet,
                interval=endpoint.interval,
            )
            for endpoint in record.endpoints
        ),
    )
