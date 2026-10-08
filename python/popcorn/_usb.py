"""The virtual USB host of a device controller: cable, enumeration, descriptors, control
requests, CDC ACM streams, HID functions and USB Audio 2.0 functions, in virtual time.

`Periph(host=UsbHostOptions())` on OTG_FS in the project plugs a full-speed host into the
controller, `machine.periph.OTG_FS.host` is its `UsbHost`. Everything below Python runs in
model events: no Python call per transaction.

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

from __future__ import annotations

import enum
import functools
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._errors import ConfigError, EmulationError, ModelError, UsbError, UsbStall, UsbTimeout
from popcorn._streams import Poster, PtyBridge, Waiter, cdc_key, written
from popcorn.units import VirtualDuration, VirtualTime, ms, s

if TYPE_CHECKING:
    from popcorn._refs import Pending

LANGUAGES = 0
DEBOUNCE = ms(100)
RESET = ms(10)
TIMEOUT = s(5)
PARITIES = "NOEMS"
STOP_BITS = (1, 1.5, 2)
DATA_BITS = (5, 6, 7, 8, 16)
FEATURE = 3
_MUTE = 1
_VOLUME = 2


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


@dataclass(frozen=True, slots=True)
class CdcPacket:
    """One packet a CDC ACM function sent, or the part of it not read yet.

    Attributes:
        data: Its bytes.
        at: Virtual time of the transaction that carried it.
    """

    data: bytes
    at: VirtualTime


@dataclass(frozen=True, slots=True)
class CdcLineCoding:
    """The line coding of a CDC ACM function, CDC PSTN 1.2 table 17, in pyserial's terms.

    Attributes:
        baud: Bits per second.
        data_bits: 5, 6, 7, 8 or 16.
        parity: `N`, `O`, `E`, `M` or `S`.
        stop_bits: 1, 1.5 or 2.
    """

    baud: int = 115200
    data_bits: int = 8
    parity: str = "N"
    stop_bits: float = 1


class CdcStream:
    """A CDC ACM function of a USB device, `m.periph.OTG_FS.host.cdc()`: bytes in and out in
    virtual time.

    The host's CDC ACM driver binds each ACM function it finds in the configuration. At
    configuration it sends SET_LINE_CODING 115200 8N1, then SET_CONTROL_LINE_STATE with the DTR
    and RTS of `UsbHostOptions`, and sends nothing else on its own: DTR stays as set until
    `set_control_line_state` changes it. It keeps one bulk IN transfer of one packet pending
    and polls the notification endpoint for SERIAL_STATE.

    Bytes written wait in a queue until the device is configured and has answered the control
    line state, then go out from the next frame in transfers of up to 20 packets, the write size
    of Linux cdc-acm, a zero-length packet after a full last one. The zero-length packet departs
    from Linux, which sends it only for devices with its SEND_ZERO_PACKET quirk. A bulk IN STALL
    is cleared as Linux does, with CLEAR_FEATURE(ENDPOINT_HALT), and reading goes on. A bulk OUT
    STALL stops writing, with a warning on the controller's log. Bytes the device sends are
    queued with the time of their packet, up to 16 MiB, the oldest dropped first and counted in
    `dropped`. Deterministic: the same writes at the same virtual times give the same packets at
    the same times.

    The stream outlives the driver: it exists before the device is configured, an unplug keeps
    the bytes received and the bytes not sent yet, and a replug binds the same stream again.
    `Machine.restore` drops the bytes received and not read yet, and puts back the bytes not
    yet taken by the device as the snapshot had them, bound or not.

    A pty bridge owns what the device sends. Once the function is bridged, by
    `Periph(connect="pty")` or by `m.board.pty(name)`, `read` and `read_packets`
    raise `RuntimeError("OTG_FS is bridged to /dev/pts/N")`, the key and path of the bridge,
    for the life of the machine. `write` is not refused, its bytes mix with the pty client's.
    `Periph(connect="pty")` bridges at build, for every test of the project. A test that reads the
    stream leaves `connect` out, and a test that wants the pty calls `m.board.pty(name)`,
    which bridges on first call.
    """

    __slots__ = (
        "__weakref__",
        "_bridges",
        "_key",
        "_link",
        "_pending",
        "_post",
        "_wait",
        "function",
        "name",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.CdcLink,
        wait: Waiter,
        bridges: Mapping[str, PtyBridge],
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.cdc()` instead."""
        self.name = name
        self.function = function
        self._key = cdc_key(name, function)
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._post = post
        self._pending: set[int] = set()

    @property
    def bound(self) -> bool:
        """Whether the host has bound its CDC ACM driver to the function.

        True from the moment the device is configured, before the driver's SET_LINE_CODING and
        SET_CONTROL_LINE_STATE went out: `dtr`, `rts` and `line_coding` are not the device's yet,
        and bytes written still wait. `ready` tells when they are. False again once the device is
        unplugged, reset by the bus or restored to a snapshot taken before configuration.
        """
        return self._link.bound()

    @property
    def ready(self) -> bool:
        """Whether the function is bound and the driver's SET_LINE_CODING and
        SET_CONTROL_LINE_STATE of configuration have ended, whatever their status.

        From then on `dtr`, `rts` and `line_coding` hold what the device acknowledged and bytes
        written go out. With `UsbHostOptions.dtr` False the SET_CONTROL_LINE_STATE still goes
        out, with DTR off, so ready means the same. A device that stalls the request is ready
        too, with `dtr` False: check `dtr` when the test needs it set. A snapshot keeps it.
        """
        return self._link.bound() and self._link.ready()

    @property
    def available(self) -> int:
        """Bytes the device sent, not read yet."""
        return self._link.available()

    @property
    def queued(self) -> int:
        """Bytes written and not yet taken by the device, the transfer in progress included."""
        return self._link.queued()

    @property
    def dropped(self) -> int:
        """Received bytes dropped because the queue held its limit, since the machine was built
        or the last `Machine.restore`."""
        return self._link.dropped()

    @property
    def line_coding(self) -> CdcLineCoding:
        """The last line coding the device acknowledged, 115200 8N1 before any. A parity code
        above 4 reads as `?`, a stop bits code above 2 as 0."""
        record = self._link.line_coding()
        return CdcLineCoding(
            baud=record.baud,
            data_bits=record.data_bits,
            parity=PARITIES[record.parity] if record.parity < len(PARITIES) else "?",
            stop_bits=STOP_BITS[record.stop_bits] if record.stop_bits < len(STOP_BITS) else 0,
        )

    @property
    def dtr(self) -> bool:
        """DTR in the last SET_CONTROL_LINE_STATE the device acknowledged."""
        return bool(self._link.line_state() & 1)

    @property
    def rts(self) -> bool:
        """RTS in the last SET_CONTROL_LINE_STATE the device acknowledged."""
        return bool(self._link.line_state() & 2)

    @property
    def serial_state(self) -> int:
        """The UART state bitmap of the last SERIAL_STATE notification, CDC PSTN 1.2 table 31,
        0 before any."""
        return self._link.serial_state()

    def write(self, data: bytes, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue bytes for the device. They go out as the machine runs, once it is configured.
        Posted at the call as one command, as `UartStream.write`: await the result for the time
        they were queued at.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link = self._link
        data = bytes(data)
        return written(self._post(lambda: link.send(data), at))

    async def read(self, n: int, *, for_: VirtualDuration | None = None) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them, as
        `UartStream.read`.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed or the core went idle first.

        Raises:
            ValueError: `n` is negative.
            RuntimeError: The function is bridged to a pty, which takes what the device sends:
                `Periph(connect="pty")` or `m.board.pty(name)`.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        self._check_read(n)
        if self._link.available() < n:
            await self._wait(lambda: self._link.available() >= n, for_)
        return self._link.drain(n)

    async def read_packets(self, n: int, *, for_: VirtualDuration | None = None) -> list[CdcPacket]:
        """As `read`, `n` packets with their times. The rest of a packet a `read` split counts
        as one."""
        self._check_read(n)
        if self._link.packets() < n:
            await self._wait(lambda: self._link.packets() >= n, for_)
        return [
            CdcPacket(bytes(record.data), VirtualTime(record.at_ps))
            for record in self._link.drain_packets(n)
        ]

    async def wait_ready(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until `ready`, or `for_` elapsed.

        `UsbHost.wait_configured` returns before the driver's requests end, 3 ms before on the
        TinyUSB 0.21.0 example. Inside `running()`, this awaits the background run instead of
        driving the machine.

        Raises:
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed or the core
                went idle. Its status is `unbound` while no driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if not self.ready:
            await self._wait(lambda: self.ready, for_)
        if not self.ready:
            status = "not_ready" if self.bound else "unbound"
            raise UsbTimeout(
                f"{self.name} CDC function {self.function}: not ready, {status}", status=status
            )

    async def set_control_line_state(self, *, dtr: bool = True, rts: bool = True) -> VirtualTime:
        """Send SET_CONTROL_LINE_STATE and run the machine until it ends. The only way DTR
        drops.

        Returns:
            The virtual time the request was seen ended.

        Raises:
            UsbError: No CDC ACM driver is bound to the function, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        link = self._link
        transfer, at = await self._post(lambda: link.set_control_line_state(dtr, rts))
        return await self._request(
            transfer, f"SET_CONTROL_LINE_STATE dtr={int(dtr)} rts={int(rts)}", at
        )

    async def set_line_coding(
        self, baud: int = 115200, data_bits: int = 8, parity: str = "N", stop_bits: float = 1
    ) -> VirtualTime:
        """Send SET_LINE_CODING and run the machine until it ends, the virtual time it was seen
        ended as the result.

        Args:
            baud: Bits per second, 1 to 0xffffffff.
            data_bits: 5, 6, 7, 8 or 16.
            parity: `N`, `O`, `E`, `M` or `S`.
            stop_bits: 1, 1.5 or 2.

        Raises:
            ValueError: A field is out of range.
            UsbError: No CDC ACM driver is bound to the function, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        if not 1 <= baud <= 0xFFFFFFFF:
            raise ValueError(f"baud is 1 to 0xffffffff, not {baud}")
        if data_bits not in DATA_BITS:
            raise ValueError(f"data_bits is one of {DATA_BITS}, not {data_bits}")
        if len(parity) != 1 or parity not in PARITIES:
            raise ValueError(f"parity is one of {', '.join(PARITIES)}, not {parity!r}")
        if stop_bits not in STOP_BITS:
            raise ValueError(f"stop_bits is one of {STOP_BITS}, not {stop_bits}")
        link = self._link
        stop_code, parity_code = STOP_BITS.index(stop_bits), PARITIES.index(parity)
        transfer, at = await self._post(
            lambda: link.set_line_coding(baud, stop_code, parity_code, data_bits)
        )
        return await self._request(
            transfer, f"SET_LINE_CODING {baud} {data_bits}{parity}{stop_bits:g}", at
        )

    def discard(self) -> None:
        """Forget the requests waited for, at a restore."""
        self._pending.clear()

    def _check_read(self, n: int) -> None:
        bridge = self._bridges.get(self._key)
        if bridge is not None:
            raise RuntimeError(f"{self._key} is bridged to {bridge.path}")
        if n < 0:
            raise ValueError(f"cannot read {n} bytes")

    async def _request(self, transfer: int, label: str, posted_at: VirtualTime) -> VirtualTime:
        where = f"{self.name} CDC function {self.function}"
        if transfer == 0:
            raise UsbError(f"{where}: {label} with no CDC ACM driver bound", status="unbound")
        self._pending.add(transfer)
        results: list[_core.UsbTransferStatus] = []

        def ended() -> bool:
            if not results and transfer in self._pending:
                status = self._link.take_result(transfer)
                if status is not None:
                    results.append(status)
            return bool(results)

        done_at = posted_at
        try:
            if not ended():
                done_at = await self._wait(ended, None)
        finally:
            self._pending.discard(transfer)
            if not results:
                self._link.forget(transfer)
        if not results:
            raise EmulationError(f"{where}: the run ended before {label} did")
        status = results[0]
        if status is _core.UsbTransferStatus.completed:
            return done_at
        message = f"{where}: {label} ended {status.name}"
        if status is _core.UsbTransferStatus.stalled:
            raise UsbStall(message, status=status.name)
        if status is _core.UsbTransferStatus.timed_out:
            raise UsbTimeout(message, status=status.name)
        raise UsbError(message, status=status.name)

    def __repr__(self) -> str:
        return f"CdcStream({self.name}, {self.function})"


@dataclass(frozen=True, slots=True)
class HidReport:
    """One input report a HID function sent on its interrupt IN endpoint.

    Attributes:
        data: Its bytes as the packet carried them, the report ID first when the device numbers
            its reports, as hidraw reads them.
        at: Virtual time of the transaction that carried it.
    """

    data: bytes
    at: VirtualTime


class HidStream:
    """A HID interface of a USB device, `m.periph.OTG_FS.host.hid()`: input reports with their
    times, output and feature reports, in virtual time.

    The host's HID driver binds each interface of class 0x03 whose HID descriptor announces a
    report descriptor. At configuration it probes as Linux usbhid does: SET_IDLE with duration 0
    for every report, then GET_DESCRIPTOR of the report descriptor, tried up to 4 times while it
    comes back short or fails. Once the descriptor is read it keeps one interrupt IN transfer of
    one packet pending, polled every bInterval frames, as if hidraw were open: an input report is
    one packet. An interrupt IN STALL is cleared as usbhid does, with
    CLEAR_FEATURE(ENDPOINT_HALT), and reading goes on. A failed descriptor read leaves the
    function idle, with a warning on the controller's log.

    Reports follow hidraw: the report ID comes first, and an ID of 0, for a device that does not
    number its reports, is not sent. Output reports go out one at a time on the interrupt OUT
    endpoint when the interface has one, else as SET_REPORT(Output) on the default pipe. Input
    reports are queued up to 65536, the oldest dropped first and counted in `dropped`.

    The stream outlives the driver: it exists before the device is configured, an unplug keeps
    the reports received and the output reports not sent yet, and a replug binds the same stream
    again. `Machine.restore` drops the input reports not read yet, and puts back the output
    reports not yet taken by the device as the snapshot had them, bound or not.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_pending",
        "_post",
        "_wait",
        "_writes",
        "function",
        "name",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.HidLink,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.hid()` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()
        self._writes: set[int] = set()

    @property
    def bound(self) -> bool:
        """Whether the host has bound its HID driver to the interface.

        True from the moment the device is configured, before SET_IDLE and the report descriptor
        read: no input report is polled and output reports still wait. `ready` tells when they
        start. False again once the device is unplugged, reset by the bus or restored to a
        snapshot taken before configuration.
        """
        return self._link.bound()

    @property
    def ready(self) -> bool:
        """Whether the interface is bound, the driver has read the report descriptor and has
        started polling.

        At that point the driver has submitted its first interrupt IN transfer and output reports
        may go out. A function whose descriptor read failed stays idle and never becomes ready.
        """
        return self._link.bound() and self._link.ready()

    @property
    def report_descriptor(self) -> bytes:
        """The report descriptor the driver read, raw, empty before. An unplug keeps it."""
        return self._link.report_descriptor()

    @property
    def available(self) -> int:
        """Input reports received, not read yet."""
        return self._link.available()

    @property
    def queued(self) -> int:
        """Output reports written and not yet taken by the device, the one in progress
        included."""
        return self._link.queued()

    @property
    def dropped(self) -> int:
        """Input reports dropped because the queue held its limit, since the machine was built
        or the last `Machine.restore`."""
        return self._link.dropped()

    async def wait_ready(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until `ready`, or `for_` elapsed, as `CdcStream.wait_ready`.

        Raises:
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed or the core
                went idle. Its status is `unbound` while no driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if not self.ready:
            await self._wait(lambda: self.ready, for_)
        if not self.ready:
            status = "not_ready" if self.bound else "unbound"
            raise UsbTimeout(f"{self._where}: not ready, {status}", status=status)

    async def read_reports(self, n: int, *, for_: VirtualDuration | None = None) -> list[HidReport]:
        """Run the machine until `n` input reports arrived, or `for_` elapsed, and take them.

        Returns:
            Up to `n` reports, oldest first, fewer when `for_` elapsed or the core went idle.

        Raises:
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if n < 0:
            raise ValueError(f"cannot read {n} reports")
        if self._link.available() < n:
            await self._wait(lambda: self._link.available() >= n, for_)
        return [
            HidReport(bytes(record.data), VirtualTime(record.at_ps))
            for record in self._link.drain(n)
        ]

    def write_report(self, data: bytes, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Send an output report and run the machine until the device took it.

        The report is queued at the call as one command, at the next block boundary or the
        first one at or after `at`, as `UartStream.write`. Awaiting the result waits for the
        device and returns the time the report was queued at. It waits behind the output
        reports written before, and until the device is configured and its report descriptor
        read. When the await raises EmulationError or is cancelled, the report stays queued and
        is sent later, as the machine runs: only its result is dropped.

        Args:
            data: The report as hidraw takes it: the report ID first, 0 when the device does not
                number its reports, which is then not sent.

        Raises:
            ValueError: `data` is shorter than 2 bytes, as hidraw refuses, or carries more than
                0xffff bytes on the bus, the ID 0 dropped.
            UsbStall: The device answered STALL.
            UsbTimeout: The SET_REPORT did not end within the host's timeout.
            UsbError: The transfer ended otherwise, a detach included.
            EmulationError: The run ended before the device took the report.
            RuntimeError: Called from a handler or a model.
        """
        data = bytes(data)
        if len(data) < 2:
            raise ValueError(f"an output report is at least 2 bytes, not {len(data)}")
        stage = len(data) - 1 if data[0] == 0 else len(data)
        if stage > 0xFFFF:
            raise ValueError(
                f"an output report carries at most 0xffff bytes on the bus, not {stage}"
            )
        link = self._link
        report = data[0]
        posted = self._post(
            lambda: link.write(data),
            at,
            cancel=False,
            follow=lambda queued: self._taken(queued, report),
        )
        return cast("Pending[VirtualTime]", posted)

    async def _taken(self, queued: tuple[int, VirtualTime], report: int) -> VirtualTime:
        write, at = queued
        self._writes.add(write)
        results: list[_core.UsbTransferStatus] = []

        def ended() -> bool:
            if not results and write in self._writes:
                status = self._link.take_written(write)
                if status is not None:
                    results.append(status)
            return bool(results)

        try:
            if not ended():
                await self._wait(ended, None)
        finally:
            self._writes.discard(write)
            if not results:
                self._link.forget_written(write)
        label = f"output report {report}"
        if not results:
            raise EmulationError(f"{self._where}: the run ended before {label} did")
        _raise_for(results[0], f"{self._where}: {label}")
        return at

    async def get_feature(self, report_id: int, length: int) -> UsbData:
        """Send GET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCGFEATURE.

        Args:
            report_id: Report ID, 0 when the device does not number its reports.
            length: Bytes asked for, the report ID byte included. For ID 0 the request asks one
                byte fewer, the ID is not on the bus.

        Returns:
            The report, its ID first: the bytes the device sent, after a 0 for ID 0, with the
            virtual time the request was seen ended as `.time`.

        Raises:
            ValueError: `report_id` is not 0 to 255, or `length` not 1 to 0xffff.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        if not 0 <= report_id <= 0xFF:
            raise ValueError(f"a report ID is 0 to 255, not {report_id}")
        if not 1 <= length <= 0xFFFF:
            raise ValueError(f"length is 1 to 0xffff, not {length}")
        w_length = length - 1 if report_id == 0 else length
        link = self._link
        transfer, at = await self._post(lambda: link.get_report(FEATURE, report_id, w_length))
        reply, done_at = await self._request(transfer, f"GET_REPORT feature {report_id}", at)
        return UsbData((b"\x00" if report_id == 0 else b"") + bytes(reply.data), done_at)

    async def set_feature(self, data: bytes) -> VirtualTime:
        """Send SET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCSFEATURE, the virtual time it was seen ended as the result.

        Args:
            data: The report, its ID first, 0 when the device does not number its reports, which
                is then not sent.

        Raises:
            ValueError: `data` is empty, or its data stage longer than 0xffff bytes.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        data = bytes(data)
        if not data:
            raise ValueError("a feature report has at least its report ID byte")
        stage = data[1:] if data[0] == 0 else data
        if len(stage) > 0xFFFF:
            raise ValueError(f"a data stage is at most 0xffff bytes, not {len(stage)}")
        link = self._link
        transfer, at = await self._post(lambda: link.set_report(FEATURE, data[0], stage))
        _, done_at = await self._request(transfer, f"SET_REPORT feature {data[0]}", at)
        return done_at

    def discard(self) -> None:
        """Forget the requests and reports waited for, at a restore."""
        self._pending.clear()
        self._writes.clear()

    @property
    def _where(self) -> str:
        return f"{self.name} HID function {self.function}"

    async def _request(
        self, transfer: int, label: str, posted_at: VirtualTime
    ) -> tuple[_core.HidReplyRecord, VirtualTime]:
        if transfer == 0:
            raise UsbError(f"{self._where}: {label} with no HID driver bound", status="unbound")
        self._pending.add(transfer)
        results: list[_core.HidReplyRecord] = []

        def ended() -> bool:
            if not results and transfer in self._pending:
                reply = self._link.take_result(transfer)
                if reply is not None:
                    results.append(reply)
            return bool(results)

        done_at = posted_at
        try:
            if not ended():
                done_at = await self._wait(ended, None)
        finally:
            self._pending.discard(transfer)
            if not results:
                self._link.forget(transfer)
        if not results:
            raise EmulationError(f"{self._where}: the run ended before {label} did")
        _raise_for(results[0].status, f"{self._where}: {label}")
        return results[0], done_at

    def __repr__(self) -> str:
        return f"HidStream({self.name}, {self.function})"


@dataclass(frozen=True, slots=True)
class UacFormat:
    """The PCM format of an audio stream, from its AS_GENERAL and Type I format descriptors.

    Attributes:
        channels: bNrChannels: samples per frame of audio, interleaved.
        bytes_per_sample: bSubslotSize: bytes one sample takes on the bus, 1 to 4.
        bits: bBitResolution: bits of the subslot that carry the sample.
    """

    channels: int
    bytes_per_sample: int
    bits: int


@dataclass(frozen=True, slots=True)
class UacFrame:
    """One isochronous IN packet of a microphone stream, decoded.

    Attributes:
        samples: Samples, interleaved, each the signed subslot as the bus carried it. Empty for
            a gap: the device answered an empty packet, or none.
        at: Virtual time of its transaction, whether the device answered or not.
    """

    samples: tuple[int, ...]
    at: VirtualTime


@dataclass(frozen=True, slots=True)
class UacFeatureUnit:
    """A feature unit of an audio function, with the values the host last read or set.

    Index 0 of each tuple is the master channel, index n the n-th logical channel.

    Attributes:
        unit: bUnitID.
        channels: Logical channels, the master channel excluded.
        controls: bmaControls per channel: 2 bits per control, bit 0 of a pair readable, bit 1
            writable, mute in bits 1:0, volume in bits 3:2.
        mute: Mute per channel, None where it is not readable or was not read.
        volume: Volume per channel in 1/256 dB, None where it is not readable or was not read.
        volume_range: (minimum, maximum, resolution) of the volume per channel in 1/256 dB, as
            the probe's GET_RANGE gave it, None where not read.
    """

    unit: int
    channels: int
    controls: tuple[int, ...]
    mute: tuple[bool | None, ...]
    volume: tuple[int | None, ...]
    volume_range: tuple[tuple[int, int, int] | None, ...]


class UacStream:
    """One streaming direction of a USB Audio 2.0 function: `audio.speaker` or `audio.mic`.

    The stream is the function's first audio streaming interface in that direction, run at its
    first alternate setting with an isochronous endpoint and a Type I PCM format. `start`
    selects that setting, `stop` goes back to setting 0. The rate is the clock's GET_CUR, so a
    device with a fixed clock runs at its own rate, and the host's frames set it: 1 ms per
    frame, no drift. A real host and device drift apart and resync through feedback or
    adaptive clocking, which the emulation never exercises.

    `interface`, `alternate`, `format`, `sample_rates` and `rate` keep their last values after an
    unplug or a bus reset, until a driver binds again or a restore.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_mic",
        "_pending",
        "_post",
        "_wait",
        "function",
        "name",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.UacLink,
        mic: bool,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.audio().speaker` or `.mic` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._mic = mic
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()

    @property
    def bound(self) -> bool:
        """Whether the host has bound its audio driver to the function and found this stream."""
        return self._link.bound() and self._record().present

    @property
    def interface(self) -> int | None:
        """bInterfaceNumber of the audio streaming interface, None while not found."""
        record = self._record()
        return record.interface if record.present else None

    @property
    def alternate(self) -> int | None:
        """The alternate setting `start` selects, None while not found."""
        record = self._record()
        return record.alternate if record.present else None

    @property
    def format(self) -> UacFormat | None:
        """The PCM format of that setting, None while not found."""
        record = self._record()
        if not record.present:
            return None
        return UacFormat(record.channels, record.subslot, record.bits)

    @property
    def sample_rates(self) -> tuple[int, ...]:
        """Sample rates of the clock's GET_RANGE in Hz, empty before it is read. A continuous
        range shows as its two ends."""
        return tuple(self._record().rates)

    @property
    def rate(self) -> int:
        """The rate the stream runs at in Hz: the clock's GET_CUR, else the highest of
        `sample_rates`, 0 before the clock is read."""
        return self._record().rate

    @property
    def running(self) -> bool:
        """Whether the setting is selected and an isochronous packet goes every frame."""
        return self._record().running

    async def start(self) -> VirtualTime:
        """Select the stream's alternate setting and run the machine until the device
        acknowledged it, the virtual time that was seen as the result. The first isochronous
        packet goes 2 frames after the pipe opens.

        It sends what Linux sends when it prepares a stream: the clock's validity and GET_CUR
        of its frequency, SET_CUR when the rate differs and the clock takes it, the validity
        again, then SET_INTERFACE. A clock another running stream holds is not read again. A
        running stream returns at once.

        Raises:
            UsbError: No audio driver is bound, status `unbound`, the function has no stream in
                this direction, `absent`, its clock rate is unknown, `no_rate`, a start is in
                progress or the other stream runs on the same clock at another rate, `busy`, or a
                request ended neither completed, stalled nor timed out.
            UsbStall: The device stalled SET_INTERFACE or SET_CUR.
            UsbTimeout: A request did not end within the host's timeout.
            EmulationError: The run ended before the device acknowledged.
            RuntimeError: Called from a handler or a model.
        """
        if self.running:
            _, now = await self._post(lambda: None)
            return now
        link, mic = self._link, self._mic
        operation, posted_at = await self._post(lambda: link.start(mic))
        if operation == 0:
            if not self._link.bound():
                status = "unbound"
            elif not self._record().present:
                status = "absent"
            elif self._record().rate == 0:
                status = "no_rate"
            else:
                status = "busy"
            raise UsbError(f"{self._where}: cannot start, {status}", status=status)
        self._pending.add(operation)
        results: list[_core.UsbTransferStatus] = []

        def ended() -> bool:
            if not results and operation in self._pending:
                status = self._link.take_result(operation)
                if status is not None:
                    results.append(status)
            return bool(results)

        done_at = posted_at
        try:
            if not ended():
                done_at = await self._wait(ended, None)
        finally:
            self._pending.discard(operation)
            if not results:
                self._link.forget(operation)
        if not results:
            raise EmulationError(f"{self._where}: the run ended before the start did")
        _raise_for(results[0], f"{self._where}: start")
        return done_at

    def stop(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Stop the stream, posted at the call as one command: at once while the machine is
        parked, at the next block boundary while it runs, or at the first one at or after `at`.
        Await the result for the virtual time it ran at.

        The host closes the pipe then: no isochronous packet goes from then on, and a speaker's
        samples not yet sent are dropped, whether it ran or not. SET_INTERFACE 0 is queued in the
        host state and goes on the bus as the machine runs, after any control request already
        queued. A start in progress is cancelled. A stream neither running nor starting sends no
        request.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link, mic = self._link, self._mic
        return written(self._post(lambda: link.stop(mic), at))

    def discard(self) -> None:
        """Forget the starts waited for, at a restore."""
        self._pending.clear()

    @property
    def _where(self) -> str:
        return f"{self.name} audio function {self.function} {'mic' if self._mic else 'speaker'}"

    def _record(self) -> _core.UacStreamRecord:
        return self._link.stream(self._mic)


class UacSpeaker(UacStream):
    """The speaker of a USB Audio 2.0 function, `host.audio().speaker`: PCM to the device on
    its isochronous OUT endpoint.

    Each frame while running carries the rate's samples per channel, rate / 1000, a fraction
    spread as USB Audio 2.0 3.16.2 does: at 44.1 kHz 44 samples, one frame in 10 carries 45.
    Samples written fill the packets in order, silence pads a packet past the last one. They
    wait while the speaker is stopped, are dropped by `stop`, and are model state: a snapshot
    keeps them, bound or not, and a restore puts them back.
    """

    __slots__ = ()

    @property
    def queued(self) -> int:
        """Samples written and not yet put in a packet, interleaved values."""
        return self._link.queued()

    @property
    def sending(self) -> bool:
        """Whether a packet carrying written samples waits to go on the bus. The first packet
        of a stream waits two frames after `start` opens the pipe."""
        return self._link.sending()

    @property
    def sent_at(self) -> VirtualTime | None:
        """Virtual time of the last packet that carried written samples, None before any."""
        at = self._link.sent_at()
        return None if at is None else VirtualTime(at)

    def write(
        self, samples: Sequence[int], *, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue samples for the device, interleaved, a whole number of frames of audio. Posted
        at the call as one command, as `UartStream.write`: await the result for the time they
        were queued at.

        Args:
            samples: Signed samples of the format's subslot, `bytes_per_sample` bytes, as the bus
                carries them. Before the format is known any 32-bit value is taken and its low
                bytes are sent.
            at: The first block boundary at or after it queues them, None for the next one.

        Raises:
            ValueError: A sample is out of range, or the count is not a multiple of the channels.
            RuntimeError: Called from a handler or a model.
        """
        values = list(samples)
        layout = self.format
        width = 4 if layout is None else layout.bytes_per_sample
        top = 1 << (8 * width - 1)
        for sample in values:
            if isinstance(sample, bool) or not -top <= sample < top:
                raise ValueError(f"a {width}-byte sample is {-top} to {top - 1}, not {sample!r}")
        if layout is not None and len(values) % layout.channels:
            raise ValueError(
                f"{len(values)} samples are not whole frames of {layout.channels} channels"
            )
        link = self._link
        return written(self._post(lambda: link.write(values), at))

    async def drain(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until every sample written went out in an isochronous packet, or
        `for_` elapsed, as `UartStream.read` runs it. `sent_at` is then the time of the packet
        that carried the last sample. The host sends the first packet of a stream two frames
        after `start` opens the pipe, so a drain right after `start` waits for it.

        Raises:
            UsbError: Samples are queued and the speaker is not running, or while the call
                waited a `stop` dropped samples, or a `stop`, an unplug or a bus reset dropped
                the packet not sent yet. Status `stopped`.
            UsbTimeout: Samples are still queued or in a packet not sent yet when the run ends:
                `for_` elapsed or the core went idle. Status `queued`.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        flushes = self._link.flushes()
        if self._link.queued() and not self.running:
            raise UsbError(
                f"{self._where}: {self._link.queued()} samples queued and the speaker stopped",
                status="stopped",
            )
        if self._unsent():
            await self._wait(lambda: not self._unsent() or self._link.flushes() != flushes, for_)
        if self._link.flushes() != flushes:
            raise UsbError(f"{self._where}: stopped while draining", status="stopped")
        if self._unsent():
            unsent = (
                f"{self._link.queued()} samples still queued"
                if self._link.queued()
                else "the last packet not sent yet"
            )
            raise UsbTimeout(f"{self._where}: {unsent}", status="queued")

    def _unsent(self) -> bool:
        return self._link.queued() != 0 or self._link.sending()

    def __repr__(self) -> str:
        return f"UacSpeaker({self.name}, {self.function})"


class UacMic(UacStream):
    """The microphone of a USB Audio 2.0 function, `host.audio().mic`: PCM from the device on
    its isochronous IN endpoint.

    While running, the host asks for one packet of wMaxPacketSize every frame and keeps each as
    a `UacFrame` with its time, up to 65536 frames, the oldest dropped first and counted in
    `dropped`. A frame the device left empty or did not answer is kept as a gap, with no
    samples, so frames stay 1 ms apart. `Machine.restore` drops the frames not read yet.
    """

    __slots__ = ()

    @property
    def available(self) -> int:
        """Frames received, not read yet."""
        return self._link.available()

    @property
    def dropped(self) -> int:
        """Frames dropped because the queue held its limit, since the machine was built or the
        last `Machine.restore`."""
        return self._link.dropped()

    def clear(self) -> None:
        """Drop the frames received and not read yet, at once, as a restore does, from any
        thread, the machine running or not. The next frame read is newer than the call."""
        self._link.clear()

    async def read_frames(self, n: int, *, for_: VirtualDuration | None = None) -> list[UacFrame]:
        """Run the machine until `n` frames arrived, or `for_` elapsed, and take them, as
        `AudioStream.read_frames`.

        Returns:
            Up to `n` frames, oldest first, fewer when `for_` elapsed or the core went idle.

        Raises:
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if n < 0:
            raise ValueError(f"cannot read {n} frames")
        if self._link.available() < n:
            await self._wait(lambda: self._link.available() >= n, for_)
        return [
            UacFrame(tuple(record.samples), VirtualTime(record.at_ps))
            for record in self._link.drain(n)
        ]

    def __repr__(self) -> str:
        return f"UacMic({self.name}, {self.function})"


class UacFunction:
    """A USB Audio 2.0 function of a USB device, `m.periph.OTG_FS.host.audio()`: a speaker, a
    microphone and feature units, in virtual time.

    The host's audio driver binds each audio control interface of protocol 0x20 with the
    interfaces of its association. At configuration it probes as Linux snd-usb-audio does:
    for each alternate setting of each streaming interface, GET_RANGE of the clock's
    frequency, SET_INTERFACE 0, the clock's validity and frequency, SET_CUR of the highest
    rate when the clock takes it, SET_INTERFACE to the setting, then SET_INTERFACE 0. Then, for
    each feature unit an output terminal reaches, mute and volume: GET_RANGE of the volume, the
    resolution check Linux makes with SET_CUR, and GET_CUR of each channel. A failed or stalled
    request is not fatal, as in Linux: a stalled SET_CUR of the frequency leaves the clock's
    rate. The function outlives the driver: an unplug and a replug bind the same streams.

    Not modelled from Linux: a clock selector is followed through its first pin, with no GET_CUR
    or SET_CUR on it. Only mute and volume of feature units are probed: other controls, mixer,
    selector, processing and extension units and terminal connector controls send nothing. The
    early exit of Linux's mixer on an unrecoverable dB range is not mirrored. A setting whose
    isochronous endpoint has a bInterval above 1 is not streamed.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_pending",
        "_post",
        "_wait",
        "function",
        "mic",
        "name",
        "speaker",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.UacLink,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.audio()` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()
        self.speaker = UacSpeaker(name, function, link, False, wait, post)
        self.mic = UacMic(name, function, link, True, wait, post)

    @property
    def bound(self) -> bool:
        """Whether the host has bound its audio driver to the function.

        True from the moment the device is configured, before the probe requests: rates and
        feature unit values are not read yet. `ready` tells when they are. False again once the
        device is unplugged, reset by the bus or restored to a snapshot taken before
        configuration.
        """
        return self._link.bound()

    @property
    def ready(self) -> bool:
        """Whether the function is bound and the driver's probe requests have ended, whatever
        their status. From then on `sample_rates`, `rate` and `feature_units` hold what the
        device answered."""
        return self._link.bound() and self._link.ready()

    @property
    def feature_units(self) -> tuple[UacFeatureUnit, ...]:
        """Feature units in the order the probe reached them, empty before configuration."""
        return tuple(
            UacFeatureUnit(
                unit=record.id,
                channels=record.channels,
                controls=tuple(record.controls),
                mute=tuple(record.mute),
                volume=tuple(record.volume),
                volume_range=tuple(
                    None if low is None or high is None or step is None else (low, high, step)
                    for low, high, step in zip(
                        record.volume_min, record.volume_max, record.volume_res, strict=True
                    )
                ),
            )
            for record in self._link.units()
        )

    async def wait_ready(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until `ready`, or `for_` elapsed, as `CdcStream.wait_ready`.

        Raises:
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed or the core
                went idle. Its status is `unbound` while no driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if not self.ready:
            await self._wait(lambda: self.ready, for_)
        if not self.ready:
            status = "not_ready" if self.bound else "unbound"
            raise UsbTimeout(f"{self._where}: not ready, {status}", status=status)

    async def set_mute(self, unit: int, channel: int, mute: bool) -> VirtualTime:
        """Send SET_CUR of a feature unit's mute and run the machine until it ends, the virtual
        time it was seen ended as the result.

        Args:
            unit: bUnitID.
            channel: 0 for the master channel, n for the n-th logical channel.
            mute: The mute.

        Raises:
            ValueError: The function has no such unit or channel, or its mute is not writable.
            UsbError: No audio driver is bound, or the request ended neither completed, stalled
                nor timed out. Linux sends a failed SET_CUR up to 10 times, so does the host.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        return await self._set(unit, channel, _MUTE, int(bool(mute)), f"mute {int(bool(mute))}")

    async def set_volume(self, unit: int, channel: int, volume: int) -> VirtualTime:
        """Send SET_CUR of a feature unit's volume and run the machine until it ends, as
        `set_mute`.

        Args:
            unit: bUnitID.
            channel: 0 for the master channel, n for the n-th logical channel.
            volume: In 1/256 dB, -32768 to 32767.

        Raises:
            ValueError: The volume is out of range, the function has no such unit or channel,
                or its volume is not writable.
            UsbError: As `set_mute`.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        if not -0x8000 <= volume <= 0x7FFF:
            raise ValueError(f"a volume is -32768 to 32767, not {volume}")
        return await self._set(unit, channel, _VOLUME, volume, f"volume {volume}")

    def discard(self) -> None:
        """Forget the requests and starts waited for, at a restore."""
        self._pending.clear()
        self.speaker.discard()
        self.mic.discard()

    @property
    def _where(self) -> str:
        return f"{self.name} audio function {self.function}"

    async def _set(
        self, unit: int, channel: int, control: int, value: int, label: str
    ) -> VirtualTime:
        where = f"{self._where}: SET_CUR unit {unit} channel {channel} {label}"
        if not self._link.bound():
            raise UsbError(f"{where} with no audio driver bound", status="unbound")
        found = next((u for u in self.feature_units if u.unit == unit), None)
        if found is None or not 0 <= channel <= found.channels:
            raise ValueError(f"{where}: no such unit and channel")
        if not (found.controls[channel] >> (2 * (control - 1))) & 2:
            raise ValueError(f"{where}: the control is not writable")
        link = self._link
        operation, posted_at = await self._post(
            lambda: link.set_feature(unit, channel, control, value)
        )
        if operation == 0:
            status = "refused" if self._link.bound() else "unbound"
            raise UsbError(f"{where} refused by the driver, {status}", status=status)
        self._pending.add(operation)
        results: list[_core.UsbTransferStatus] = []

        def ended() -> bool:
            if not results and operation in self._pending:
                status = self._link.take_result(operation)
                if status is not None:
                    results.append(status)
            return bool(results)

        done_at = posted_at
        try:
            if not ended():
                done_at = await self._wait(ended, None)
        finally:
            self._pending.discard(operation)
            if not results:
                self._link.forget(operation)
        if not results:
            raise EmulationError(f"{where}: the run ended before it did")
        _raise_for(results[0], where)
        return done_at

    def __repr__(self) -> str:
        return f"UacFunction({self.name}, {self.function})"


def _raise_for(status: _core.UsbTransferStatus, label: str) -> None:
    if status is _core.UsbTransferStatus.completed:
        return
    message = f"{label} ended {status.name}"
    if status is _core.UsbTransferStatus.stalled:
        raise UsbStall(message, status=status.name)
    if status is _core.UsbTransferStatus.timed_out:
        raise UsbTimeout(message, status=status.name)
    raise UsbError(message, status=status.name)


class UsbData(bytes):
    """Bytes a USB request returned, and the virtual time it ended at.

    Attributes:
        time: Virtual time the request ended at.
    """

    time: VirtualTime

    def __new__(cls, data: bytes, time: VirtualTime) -> UsbData:
        """Pair `data` with the time its request ended at."""
        made = super().__new__(cls, data)
        made.time = time
        return made


class UsbHost:
    """The virtual USB host of a device controller, `m.periph.OTG_FS.host`.

    The host lives in the machine, inside the controller model: it sees the pull-up, debounces,
    resets the bus and enumerates without Python, in virtual time. Its state, the cable
    included, is in snapshots, so a snapshot taken once the device is configured restores a
    configured device. Results of control requests not collected yet are dropped by
    `Machine.restore`.

    `attached`, `state`, `address`, `configuration`, `failed_step`, the descriptors, `strings`,
    `interfaces`, `requests` and the first `cdc`, `hid` or `audio` call of a function read the
    host: they raise MachineRunning while the machine is busy, outside its callbacks. The queue
    and state properties of CDC, HID and audio streams read their link and work while it runs.
    """

    __slots__ = (
        "_audio",
        "_audio_link",
        "_bridges",
        "_cdc",
        "_cdc_link",
        "_hid",
        "_hid_link",
        "_link",
        "_pending",
        "_post",
        "_results",
        "_wait",
        "name",
    )

    def __init__(
        self,
        name: str,
        link: _core.UsbLink,
        wait: Waiter,
        post: Poster,
        bridges: Mapping[str, PtyBridge],
        cdc_link: Callable[[int], _core.CdcLink],
        hid_link: Callable[[int], _core.HidLink],
        audio_link: Callable[[int], _core.UacLink],
    ) -> None:
        """Wrap a native link. `cdc_link`, `hid_link` and `audio_link` give the link of a CDC,
        HID or audio function by index. Use `m.periph.<NAME>.host` instead."""
        self.name = name
        self._link = link
        self._wait = wait
        self._post = post
        self._bridges = bridges
        self._cdc_link = cdc_link
        self._results: dict[int, _core.UsbControlResult] = {}
        self._pending: set[int] = set()
        self._cdc: dict[int, CdcStream] = {}
        self._hid_link = hid_link
        self._hid: dict[int, HidStream] = {}
        self._audio_link = audio_link
        self._audio: dict[int, UacFunction] = {}

    @property
    def attached(self) -> bool:
        """Whether the cable is plugged.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks.
        """
        return self._link.plugged()

    async def attach(self) -> VirtualTime:
        """Plug the cable, as one command at the next block boundary, at once while the
        machine is parked: the host sees the device's pull-up, then debounces and enumerates as
        the machine runs.

        Returns:
            The virtual time the cable was plugged at.

        Raises:
            RuntimeError: Called from a handler or a model.
        """
        link = self._link
        _, at = await self._post(lambda: link.plug(True))
        return at

    def detach(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Unplug the cable, posted at the call as one command, as `UacStream.stop`: VBUS
        drops, the device is gone for the host, transfers in flight end cancelled. Await the
        result for the virtual time it ran at.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link = self._link
        return written(self._post(lambda: link.plug(False), at))

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

    @property
    def requests(self) -> tuple[UsbRequest, ...]:
        """Every control request the host sent since the machine was built, oldest first:
        enumeration, class drivers and `control` alike. Not part of snapshots."""
        return requests_of(self._link.requests())

    def cdc(self, function: int = 0) -> CdcStream:
        """The stream of a CDC ACM function of the device, the same one on later calls.

        Args:
            function: The n-th CDC ACM function in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                function. Before it is read, any index is taken.
        """
        stream = self._cdc.get(function)
        if stream is None:
            link = self._cdc_link(function)
            stream = self._cdc[function] = CdcStream(
                self.name, function, link, self._wait, self._bridges, self._post
            )
        return stream

    def hid(self, function: int = 0) -> HidStream:
        """The stream of a HID interface of the device, the same one on later calls.

        Args:
            function: The n-th HID interface in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                interface. Before it is read, any index is taken.
        """
        stream = self._hid.get(function)
        if stream is None:
            link = self._hid_link(function)
            stream = self._hid[function] = HidStream(
                self.name, function, link, self._wait, self._post
            )
        return stream

    def audio(self, function: int = 0) -> UacFunction:
        """A USB Audio 2.0 function of the device, the same one on later calls.

        Args:
            function: The n-th audio control interface in interface order, 0 for the first.

        Raises:
            ValueError: `function` is negative, or the configuration is read and has no such
                function. Before it is read, any index is taken.
        """
        found = self._audio.get(function)
        if found is None:
            link = self._audio_link(function)
            found = self._audio[function] = UacFunction(
                self.name, function, link, self._wait, self._post
            )
        return found

    async def wait_configured(self, *, for_: VirtualDuration | None = None) -> None:
        """Run the machine until the host configured the device, or `for_` elapsed.

        Inside `running()`, this awaits the background run instead of driving the machine.

        Raises:
            UsbTimeout: The device is not configured when the run ends: `for_` elapsed, the
                core went idle, or the enumeration stopped on a failed step.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        configured = UsbState.CONFIGURED.value
        link = self._link
        seen: list[tuple[int, str | None]] = []

        def settled() -> bool:
            seen[:] = [(link.state().value, link.failed_step())]
            return seen[0][0] == configured or seen[0][1] is not None

        await self._wait(settled, for_)
        if not seen:
            settled()
        state, failed = seen[0]
        if state != configured:
            status = failed or UsbState(state).name
            reason = f"enumeration failed at {failed}" if failed else f"state {status}"
            raise UsbTimeout(f"{self.name}: the device is not configured, {reason}", status=status)

    async def control(
        self, request_type: int, request: int, value: int, index: int, data_or_length: bytes | int
    ) -> UsbData:
        """Send a control request on the default pipe and run the machine until it ends.

        Args:
            request_type: bmRequestType. Bit 7 set is IN, device to host.
            request: bRequest.
            value: wValue.
            index: wIndex.
            data_or_length: wLength for an IN request or an OUT request with no data stage,
                the data stage of an OUT request.

        Returns:
            The data stage received for IN, at most wLength bytes, empty for OUT, with the
            virtual time the request ended as `.time`.

        Raises:
            ValueError: A field is out of range, an IN request is given data, or an OUT request
                a length above 0 instead of its data.
            RuntimeError: Called from a handler or a model.
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
        link = self._link
        transfer, _ = await self._post(
            lambda: link.control(request_type, request, value, index, length, data)
        )
        self._pending.add(transfer)
        try:
            await self._wait(lambda: self._collected(transfer), None)
        finally:
            self._pending.discard(transfer)
        result = self._results.pop(transfer, None)
        setup = f"{request_type:#04x} {request:#04x} {value:#06x} {index:#06x} {length}"
        if result is None:
            raise EmulationError(f"{self.name}: the run ended before control {setup} did")
        status = result.status.name
        at = VirtualTime(result.at_ps)
        if result.status is _core.UsbTransferStatus.completed:
            return UsbData(bytes(result.data) if is_in else b"", at)
        message = f"{self.name}: control {setup} ended {status} at {at}"
        if result.status is _core.UsbTransferStatus.stalled:
            raise UsbStall(message, status=status)
        if result.status is _core.UsbTransferStatus.timed_out:
            raise UsbTimeout(message, status=status)
        raise UsbError(message, status=status)

    def discard(self) -> None:
        """Drop the results not collected yet, at a restore."""
        self._results.clear()
        for stream in self._cdc.values():
            stream.discard()
        for hid in self._hid.values():
            hid.discard()
        for audio in self._audio.values():
            audio.discard()

    def _collected(self, transfer: int) -> bool:
        for result in self._link.take_results():
            if result.id in self._pending:
                self._results[result.id] = result
        return transfer in self._results

    def __repr__(self) -> str:
        return f"UsbHost({self.name})"


class UsbHosts:
    """The virtual USB hosts of a machine, one per device controller given one."""

    __slots__ = (
        "_aliases",
        "_audio_links",
        "_bridges",
        "_cdc_links",
        "_connect",
        "_hid_links",
        "_hosts",
        "_links",
        "_names",
        "_post",
        "_wait",
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UsbLink],
        names: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        post: Poster,
        bridges: Mapping[str, PtyBridge] | None = None,
    ) -> None:
        """Serve the hosts of the controllers `names` through `connect`. `bridges` are the
        machine's ptys, which CDC streams check before a read."""
        self._connect = connect
        self._names = frozenset(names)
        self._aliases = dict(aliases)
        self._wait = wait
        self._post = post
        self._bridges: Mapping[str, PtyBridge] = {} if bridges is None else bridges
        self._hosts: dict[str, UsbHost] = {}
        self._cdc_links: dict[tuple[str, int], _core.CdcLink] = {}
        self._hid_links: dict[tuple[str, int], _core.HidLink] = {}
        self._audio_links: dict[tuple[str, int], _core.UacLink] = {}
        self._links: dict[str, _core.UsbLink] = {}

    def serves(self, name: str) -> bool:
        """Whether `name`, or the controller its alias names, has a virtual host."""
        return self._aliases.get(name, name) in self._names

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
                    f"{name} has no virtual USB host: add `Periph(host=UsbHostOptions())` to its"
                    f" board entry, or pass usb={{{name!r}: UsbHostOptions()}} to Machine",
                    peripheral=name,
                )
            found = self._hosts[name] = UsbHost(
                name,
                self._links.setdefault(name, self._connect(name)),
                self._wait,
                self._post,
                self._bridges,
                functools.partial(self.cdc_link, name),
                functools.partial(self.hid_link, name),
                functools.partial(self.audio_link, name),
            )
        return found

    def cdc_link(self, name: str, function: int) -> _core.CdcLink:
        """The native link of CDC ACM function `function` of the host of controller `name`,
        shared by its stream and its pty.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"a CDC function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.cdc_functions()
        if count is not None and function >= count:
            raise ValueError(
                f"{host.name}: the device has {count} CDC ACM functions, no {function}"
            )
        key = (host.name, function)
        link = self._cdc_links.get(key)
        if link is None:
            link = self._cdc_links[key] = native.cdc(function)
        return link

    def hid_link(self, name: str, function: int) -> _core.HidLink:
        """The native link of HID function `function` of the host of controller `name`.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"a HID function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.hid_functions()
        if count is not None and function >= count:
            raise ValueError(f"{host.name}: the device has {count} HID functions, no {function}")
        key = (host.name, function)
        link = self._hid_links.get(key)
        if link is None:
            link = self._hid_links[key] = native.hid(function)
        return link

    def audio_link(self, name: str, function: int) -> _core.UacLink:
        """The native link of USB Audio 2.0 function `function` of the host of controller
        `name`.

        Raises:
            ModelError: The controller has no virtual host.
            ValueError: `function` is negative, or the configuration is read and has no such
                function.
        """
        host = self.host(name)
        if function < 0:
            raise ValueError(f"an audio function index is 0 or more, not {function}")
        native = self._links[host.name]
        count = native.audio_functions()
        if count is not None and function >= count:
            raise ValueError(f"{host.name}: the device has {count} audio functions, no {function}")
        key = (host.name, function)
        link = self._audio_links.get(key)
        if link is None:
            link = self._audio_links[key] = native.audio(function)
        return link

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
