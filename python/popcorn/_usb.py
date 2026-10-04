"""The virtual USB host of a device controller: cable, enumeration, descriptors, control
requests, CDC ACM streams and HID functions, in virtual time."""

from __future__ import annotations

import enum
import functools
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass

from popcorn import _core
from popcorn._errors import ConfigError, EmulationError, ModelError, UsbError, UsbStall, UsbTimeout
from popcorn._streams import PtyBridge, Waiter, cdc_key
from popcorn.units import VirtualDuration, VirtualTime, ms, s

LANGUAGES = 0
DEBOUNCE = ms(100)
RESET = ms(10)
TIMEOUT = s(5)
PARITIES = "NOEMS"
STOP_BITS = (1, 1.5, 2)
DATA_BITS = (5, 6, 7, 8, 16)
FEATURE = 3


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
    `connect = "pty"` in `popcorn.toml` or by `m.board.pty(name)`, `read` and `read_packets`
    raise `RuntimeError("OTG_FS is bridged to /dev/pts/N")`, the key and path of the bridge,
    for the life of the machine. `write` is not refused, its bytes mix with the pty client's.
    `connect = "pty"` bridges at build, for every test of the project. A test that reads the
    stream leaves `connect` out, and a test that wants the pty calls `m.board.pty(name)`,
    which bridges on first call.
    """

    __slots__ = (
        "__weakref__",
        "_bridges",
        "_key",
        "_link",
        "_pending",
        "_stimulus",
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
        stimulus: Callable[[], None],
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.cdc()` instead."""
        self.name = name
        self.function = function
        self._key = cdc_key(name, function)
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._stimulus = stimulus
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

    async def write(self, data: bytes) -> None:
        """Queue bytes for the device. They go out as the machine runs, once it is configured.

        Raises:
            RuntimeError: Called from another thread while a run is active.
        """
        self._stimulus()
        self._link.send(bytes(data))

    async def read(self, n: int, *, for_: VirtualDuration | None = None) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them, as
        `UartStream.read`.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed or the core went idle first.

        Raises:
            ValueError: `n` is negative.
            RuntimeError: The function is bridged to a pty, which takes what the device sends:
                `connect = "pty"` or `m.board.pty(name)`.
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

    async def set_control_line_state(self, *, dtr: bool = True, rts: bool = True) -> None:
        """Send SET_CONTROL_LINE_STATE and run the machine until it ends. The only way DTR
        drops.

        Raises:
            UsbError: No CDC ACM driver is bound to the function, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from another thread while a run is active.
        """
        self._stimulus()
        await self._request(
            self._link.set_control_line_state(dtr, rts),
            f"SET_CONTROL_LINE_STATE dtr={int(dtr)} rts={int(rts)}",
        )

    async def set_line_coding(
        self, baud: int = 115200, data_bits: int = 8, parity: str = "N", stop_bits: float = 1
    ) -> None:
        """Send SET_LINE_CODING and run the machine until it ends.

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
            RuntimeError: Called from another thread while a run is active.
        """
        if not 1 <= baud <= 0xFFFFFFFF:
            raise ValueError(f"baud is 1 to 0xffffffff, not {baud}")
        if data_bits not in DATA_BITS:
            raise ValueError(f"data_bits is one of {DATA_BITS}, not {data_bits}")
        if len(parity) != 1 or parity not in PARITIES:
            raise ValueError(f"parity is one of {', '.join(PARITIES)}, not {parity!r}")
        if stop_bits not in STOP_BITS:
            raise ValueError(f"stop_bits is one of {STOP_BITS}, not {stop_bits}")
        self._stimulus()
        await self._request(
            self._link.set_line_coding(
                baud, STOP_BITS.index(stop_bits), PARITIES.index(parity), data_bits
            ),
            f"SET_LINE_CODING {baud} {data_bits}{parity}{stop_bits:g}",
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

    async def _request(self, transfer: int, label: str) -> None:
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

        try:
            if not ended():
                await self._wait(ended, None)
        finally:
            self._pending.discard(transfer)
            if not results:
                self._link.forget(transfer)
        if not results:
            raise EmulationError(f"{where}: the run ended before {label} did")
        status = results[0]
        if status is _core.UsbTransferStatus.completed:
            return
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
        "_stimulus",
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
        stimulus: Callable[[], None],
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.hid()` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._wait = wait
        self._stimulus = stimulus
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

    async def write_report(self, data: bytes) -> None:
        """Send an output report and run the machine until the device took it.

        It waits behind the output reports written before, and until the device is configured
        and its report descriptor read. When the call raises EmulationError or is cancelled,
        the report stays queued and is sent later, as the machine runs: only its result is
        dropped.

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
            RuntimeError: Called from another thread while a run is active.
        """
        data = bytes(data)
        if len(data) < 2:
            raise ValueError(f"an output report is at least 2 bytes, not {len(data)}")
        stage = len(data) - 1 if data[0] == 0 else len(data)
        if stage > 0xFFFF:
            raise ValueError(
                f"an output report carries at most 0xffff bytes on the bus, not {stage}"
            )
        self._stimulus()
        write = self._link.write(data)
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
        label = f"output report {data[0]}"
        if not results:
            raise EmulationError(f"{self._where}: the run ended before {label} did")
        _raise_for(results[0], f"{self._where}: {label}")

    async def get_feature(self, report_id: int, length: int) -> bytes:
        """Send GET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCGFEATURE.

        Args:
            report_id: Report ID, 0 when the device does not number its reports.
            length: Bytes asked for, the report ID byte included. For ID 0 the request asks one
                byte fewer, the ID is not on the bus.

        Returns:
            The report, its ID first: the bytes the device sent, after a 0 for ID 0.

        Raises:
            ValueError: `report_id` is not 0 to 255, or `length` not 1 to 0xffff.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from another thread while a run is active.
        """
        if not 0 <= report_id <= 0xFF:
            raise ValueError(f"a report ID is 0 to 255, not {report_id}")
        if not 1 <= length <= 0xFFFF:
            raise ValueError(f"length is 1 to 0xffff, not {length}")
        self._stimulus()
        w_length = length - 1 if report_id == 0 else length
        reply = await self._request(
            self._link.get_report(FEATURE, report_id, w_length),
            f"GET_REPORT feature {report_id}",
        )
        return (b"\x00" if report_id == 0 else b"") + bytes(reply.data)

    async def set_feature(self, data: bytes) -> None:
        """Send SET_REPORT(Feature) and run the machine until it ends, as hidraw's
        HIDIOCSFEATURE.

        Args:
            data: The report, its ID first, 0 when the device does not number its reports, which
                is then not sent.

        Raises:
            ValueError: `data` is empty, or its data stage longer than 0xffff bytes.
            UsbError: No HID driver is bound to the interface, or the request ended neither
                completed, stalled nor timed out.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from another thread while a run is active.
        """
        data = bytes(data)
        if not data:
            raise ValueError("a feature report has at least its report ID byte")
        stage = data[1:] if data[0] == 0 else data
        if len(stage) > 0xFFFF:
            raise ValueError(f"a data stage is at most 0xffff bytes, not {len(stage)}")
        self._stimulus()
        await self._request(
            self._link.set_report(FEATURE, data[0], stage), f"SET_REPORT feature {data[0]}"
        )

    def discard(self) -> None:
        """Forget the requests and reports waited for, at a restore."""
        self._pending.clear()
        self._writes.clear()

    @property
    def _where(self) -> str:
        return f"{self.name} HID function {self.function}"

    async def _request(self, transfer: int, label: str) -> _core.HidReplyRecord:
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

        try:
            if not ended():
                await self._wait(ended, None)
        finally:
            self._pending.discard(transfer)
            if not results:
                self._link.forget(transfer)
        if not results:
            raise EmulationError(f"{self._where}: the run ended before {label} did")
        _raise_for(results[0].status, f"{self._where}: {label}")
        return results[0]

    def __repr__(self) -> str:
        return f"HidStream({self.name}, {self.function})"


def _raise_for(status: _core.UsbTransferStatus, label: str) -> None:
    if status is _core.UsbTransferStatus.completed:
        return
    message = f"{label} ended {status.name}"
    if status is _core.UsbTransferStatus.stalled:
        raise UsbStall(message, status=status.name)
    if status is _core.UsbTransferStatus.timed_out:
        raise UsbTimeout(message, status=status.name)
    raise UsbError(message, status=status.name)


class UsbHost:
    """The virtual USB host of a device controller, `m.periph.OTG_FS.host`.

    The host lives in the machine, inside the controller model: it sees the pull-up, debounces,
    resets the bus and enumerates without Python, in virtual time. Its state, the cable
    included, is in snapshots, so a snapshot taken once the device is configured restores a
    configured device. Results of control requests not collected yet are dropped by
    `Machine.restore`.
    """

    __slots__ = (
        "_bridges",
        "_cdc",
        "_cdc_link",
        "_hid",
        "_hid_link",
        "_link",
        "_pending",
        "_results",
        "_stimulus",
        "_wait",
        "name",
    )

    def __init__(
        self,
        name: str,
        link: _core.UsbLink,
        wait: Waiter,
        stimulus: Callable[[], None],
        bridges: Mapping[str, PtyBridge],
        cdc_link: Callable[[int], _core.CdcLink],
        hid_link: Callable[[int], _core.HidLink],
    ) -> None:
        """Wrap a native link. `cdc_link` and `hid_link` give the link of a CDC or HID function
        by index. Use `m.periph.<NAME>.host` instead."""
        self.name = name
        self._link = link
        self._wait = wait
        self._stimulus = stimulus
        self._bridges = bridges
        self._cdc_link = cdc_link
        self._results: dict[int, _core.UsbControlResult] = {}
        self._pending: set[int] = set()
        self._cdc: dict[int, CdcStream] = {}
        self._hid_link = hid_link
        self._hid: dict[int, HidStream] = {}

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

    @property
    def requests(self) -> tuple[UsbRequest, ...]:
        """Every control request the host sent since the machine was built, oldest first:
        enumeration, class drivers and `control` alike. Not part of snapshots."""
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
            for record in self._link.requests()
        )

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
                self.name, function, link, self._wait, self._bridges, self._stimulus
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
                self.name, function, link, self._wait, self._stimulus
            )
        return stream

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
        for stream in self._cdc.values():
            stream.discard()
        for hid in self._hid.values():
            hid.discard()

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
        "_bridges",
        "_cdc_links",
        "_connect",
        "_hid_links",
        "_hosts",
        "_links",
        "_names",
        "_stimulus",
        "_wait",
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UsbLink],
        names: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        stimulus: Callable[[], None],
        bridges: Mapping[str, PtyBridge] | None = None,
    ) -> None:
        """Serve the hosts of the controllers `names` through `connect`. `bridges` are the
        machine's ptys, which CDC streams check before a read."""
        self._connect = connect
        self._names = frozenset(names)
        self._aliases = dict(aliases)
        self._wait = wait
        self._stimulus = stimulus
        self._bridges: Mapping[str, PtyBridge] = {} if bridges is None else bridges
        self._hosts: dict[str, UsbHost] = {}
        self._cdc_links: dict[tuple[str, int], _core.CdcLink] = {}
        self._hid_links: dict[tuple[str, int], _core.HidLink] = {}
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
                    f"{name} has no virtual USB host: add `host = {{}}` to [board.{name}], or"
                    f" pass usb={{{name!r}: UsbHostOptions()}} to Machine",
                    peripheral=name,
                )
            found = self._hosts[name] = UsbHost(
                name,
                self._links.setdefault(name, self._connect(name)),
                self._wait,
                self._stimulus,
                self._bridges,
                functools.partial(self.cdc_link, name),
                functools.partial(self.hid_link, name),
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
