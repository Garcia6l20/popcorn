"""The virtual USB host of a device controller: cable, enumeration, descriptors, control
requests, in virtual time."""

from __future__ import annotations

import enum
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass

from popcorn import _core
from popcorn._errors import ConfigError, EmulationError, ModelError, UsbError, UsbStall, UsbTimeout
from popcorn._streams import Waiter
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
    """

    attached: bool = True
    debounce: VirtualDuration = DEBOUNCE
    reset: VirtualDuration = RESET
    timeout: VirtualDuration = TIMEOUT

    def native(self) -> _core.UsbHostConfig:
        """The options as the native machine configuration takes them."""
        config = _core.UsbHostConfig()
        config.attached = self.attached
        config.debounce_ns = self.debounce.ps // 1000
        config.reset_ns = self.reset.ps // 1000
        config.timeout_ns = self.timeout.ps // 1000
        return config


class UsbHost:
    """The virtual USB host of a device controller, `m.periph.OTG_FS.host`.

    The host lives in the machine, inside the controller model: it sees the pull-up, debounces,
    resets the bus and enumerates without Python, in virtual time. Its state, the cable
    included, is in snapshots, so a snapshot taken once the device is configured restores a
    configured device. Results of control requests not collected yet are dropped by
    `Machine.restore`.
    """

    __slots__ = ("_link", "_pending", "_results", "_stimulus", "_wait", "name")

    def __init__(
        self, name: str, link: _core.UsbLink, wait: Waiter, stimulus: Callable[[], None]
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host` instead."""
        self.name = name
        self._link = link
        self._wait = wait
        self._stimulus = stimulus
        self._results: dict[int, _core.UsbControlResult] = {}
        self._pending: set[int] = set()

    @property
    def attached(self) -> bool:
        """Whether the cable is plugged."""
        return self._link.plugged()

    async def attach(self) -> None:
        """Plug the cable: the host sees the device's pull-up, then debounces and enumerates as
        the machine runs.

        Raises:
            RuntimeError: Called from another thread while a run is active.
        """
        self._stimulus()
        self._link.plug(True)

    async def detach(self) -> None:
        """Unplug the cable: VBUS drops, the device is gone for the host, transfers in flight
        end cancelled.

        Raises:
            RuntimeError: Called from another thread while a run is active.
        """
        self._stimulus()
        self._link.plug(False)

    @property
    def state(self) -> UsbState:
        """Device state as the host sees it."""
        return UsbState(self._link.state().value)

    @property
    def address(self) -> int:
        """Address the host gave the device, 0 before SET_ADDRESS."""
        return self._link.address()

    @property
    def configuration(self) -> int:
        """bConfigurationValue the host set, 0 when not configured."""
        return self._link.configuration_value()

    @property
    def failed_step(self) -> str | None:
        """The enumeration step that failed, such as `get_configuration`, None while none did."""
        return self._link.failed_step()

    @property
    def device_descriptor(self) -> bytes:
        """The device descriptor read during enumeration, empty before."""
        return self._link.device_descriptor()

    @property
    def configuration_descriptor(self) -> bytes:
        """The configuration descriptor read during enumeration, all wTotalLength bytes once
        read, empty before."""
        return self._link.configuration_descriptor()

    @property
    def strings(self) -> dict[int, str]:
        """The strings read during enumeration by index, in the first language the device
        offers. String 0, the language list, is not a string and is left out."""
        return {
            record.index: bytes(record.bytes[2:]).decode("utf-16-le", errors="replace")
            for record in self._link.strings()
            if record.index != LANGUAGES
        }

    @property
    def interfaces(self) -> tuple[UsbInterface, ...]:
        """Every interface and alternate setting of the configuration read, in descriptor
        order, empty before it is read."""
        return tuple(_interface(record) for record in self._link.interfaces())

    async def wait_configured(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until the host configured the device, or `for_` elapsed.

        Inside `running()`, this awaits the background run instead of driving the machine.

        Raises:
            UsbTimeout: The device is not configured when the run ends: `for_` elapsed, the
                core went idle, or the enumeration stopped on a failed step.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        configured = UsbState.CONFIGURED.value
        if self._link.state().value != configured:
            await self._wait(
                lambda: (
                    self._link.state().value == configured or self._link.failed_step() is not None
                ),
                for_,
            )
        if self._link.state().value != configured:
            failed = self._link.failed_step()
            status = failed or self.state.name
            reason = f"enumeration failed at {failed}" if failed else f"state {status}"
            raise UsbTimeout(f"{self.name}: the device is not configured, {reason}", status=status)

    async def control(
        self, request_type: int, request: int, value: int, index: int, data_or_length: bytes | int
    ) -> bytes:
        """Send a control request on the default pipe and run the machine until it ends.

        Args:
            request_type: bmRequestType. Bit 7 set is IN, device to host.
            request: bRequest.
            value: wValue.
            index: wIndex.
            data_or_length: wLength for an IN request or an OUT request with no data stage,
                the data stage of an OUT request.

        Returns:
            The data stage received for IN, at most wLength bytes, empty for OUT.

        Raises:
            ValueError: A field is out of range, an IN request is given data, or an OUT request
                a length above 0 instead of its data.
            RuntimeError: Called from another thread while a run is active.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout, 5 s by default.
            UsbError: The request ended otherwise: no handshake three times in a row, more data
                than wLength, or a detach.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        for label, field, top in (
            ("request_type", request_type, 0xFF),
            ("request", request, 0xFF),
            ("value", value, 0xFFFF),
            ("index", index, 0xFFFF),
        ):
            if not 0 <= field <= top:
                raise ValueError(f"{label} is 0 to {top:#x}, not {field}")
        is_in = bool(request_type & 0x80)
        if isinstance(data_or_length, int):
            length, data = data_or_length, b""
            if not 0 <= length <= 0xFFFF:
                raise ValueError(f"wLength is 0 to 0xffff, not {length}")
            if length and not is_in:
                raise ValueError("an OUT request takes its data stage as bytes, not a length")
        else:
            data = bytes(data_or_length)
            length = len(data)
            if is_in:
                raise ValueError("an IN request takes wLength, not data")
            if length > 0xFFFF:
                raise ValueError(f"an OUT data stage is at most 0xffff bytes, not {length}")
        self._stimulus()
        transfer = self._link.control(request_type, request, value, index, length, data)
        self._pending.add(transfer)
        try:
            if not self._collected(transfer):
                await self._wait(lambda: self._collected(transfer), None)
        finally:
            self._pending.discard(transfer)
        result = self._results.pop(transfer, None)
        setup = f"{request_type:#04x} {request:#04x} {value:#06x} {index:#06x} {length}"
        if result is None:
            raise EmulationError(f"{self.name}: the run ended before control {setup} did")
        status = result.status.name
        if result.status is _core.UsbTransferStatus.completed:
            return bytes(result.data) if is_in else b""
        at = VirtualTime(result.at_ps)
        message = f"{self.name}: control {setup} ended {status} at {at}"
        if result.status is _core.UsbTransferStatus.stalled:
            raise UsbStall(message, status=status)
        if result.status is _core.UsbTransferStatus.timed_out:
            raise UsbTimeout(message, status=status)
        raise UsbError(message, status=status)

    def discard(self) -> None:
        """Drop the results not collected yet, at a restore."""
        self._results.clear()

    def _collected(self, transfer: int) -> bool:
        for result in self._link.take_results():
            if result.id in self._pending:
                self._results[result.id] = result
        return transfer in self._results

    def __repr__(self) -> str:
        return f"UsbHost({self.name})"


class UsbHosts:
    """The virtual USB hosts of a machine, one per device controller given one."""

    __slots__ = ("_aliases", "_connect", "_hosts", "_names", "_stimulus", "_wait")

    def __init__(
        self,
        connect: Callable[[str], _core.UsbLink],
        names: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        stimulus: Callable[[], None],
    ) -> None:
        """Serve the hosts of the controllers `names` through `connect`."""
        self._connect = connect
        self._names = frozenset(names)
        self._aliases = dict(aliases)
        self._wait = wait
        self._stimulus = stimulus
        self._hosts: dict[str, UsbHost] = {}

    def host(self, name: str) -> UsbHost:
        """The host of controller `name`, an alias naming the controller serving it.

        Raises:
            ModelError: The controller has no virtual host.
        """
        name = self._aliases.get(name, name)
        found = self._hosts.get(name)
        if found is None:
            if name not in self._names:
                raise ModelError(
                    f"{name} has no virtual USB host: add `host = {{}}` to [board.{name}], or"
                    f" pass usb={{{name!r}: UsbHostOptions()}} to Machine",
                    peripheral=name,
                )
            found = self._hosts[name] = UsbHost(
                name, self._connect(name), self._wait, self._stimulus
            )
        return found

    def discard(self) -> None:
        """Drop the results not collected yet of every host, at a restore."""
        for host in self._hosts.values():
            host.discard()


def checked_usb(
    usb: Mapping[str, UsbHostOptions], aliases: Mapping[str, str]
) -> dict[str, _core.UsbHostConfig]:
    """`usb` keyed by the controller serving each key, as the native configuration takes it.

    Raises:
        ConfigError: Two keys name one controller, or a duration is not above 0 where it must
            be, named as `usb.<key>`.
    """
    checked: dict[str, _core.UsbHostConfig] = {}
    for key, options in usb.items():
        name = aliases.get(key, key)
        at = f"usb.{key}"
        if name in checked:
            raise ConfigError(
                f"{at}: {name} is listed twice, once through an alias",
                key=at,
                expected="one entry per USB device controller",
            )
        if options.debounce.ps < 0 or options.reset.ps <= 0 or options.timeout.ps <= 0:
            raise ConfigError(
                f"{at}: a debounce of 0 or more, a bus reset and a timeout above 0",
                key=at,
                expected="a debounce of 0 or more, a bus reset and a timeout above 0",
            )
        checked[name] = options.native()
    return checked


def _interface(record: _core.UsbInterfaceRecord) -> UsbInterface:
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
