from __future__ import annotations

import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, override

from popcorn import _core
from popcorn._errors import UsbError, UsbStall, UsbTimeout
from popcorn._streams import (
    Poster,
    PtyBridge,
    Waiter,
    cdc_key,
    check_wait,
    cutoff,
    read_until_match,
    written,
)
from popcorn._usb.transfer import await_result
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending

PARITIES = "NOEMS"
STOP_BITS = (1, 1.5, 2)
DATA_BITS = (5, 6, 7, 8, 16)


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
    `Periph(connect="pty")` bridges at build, for every test of the machine description. A test that
    reads the stream leaves `connect` out, and a test that wants the pty calls `m.board.pty(name)`,
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
        return written(self._post(link.command_send(data), at))

    async def read(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them, as
        `UartStream.read`.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no packet past it, even one queued at the call. Already past, the
        read ends at once with the packets ready by it.

        A read that waits takes only the bytes of packets received by the time its wait
        stopped, in one native step: later packets stay queued for the next read, even those a
        free run received before this read resumed. So with `deadline` or neither, the bytes
        depend only on what the device sent, the same parked and running. Inside a free run
        `for_` counts from the boundary where the wait registered, which wall time sets.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed, `deadline` passed or the core went
                idle first.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `n` is negative.
            RuntimeError: The function is bridged to a pty, which takes what the device sends:
                `Periph(connect="pty")` or `m.board.pty(name)`.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        self._check_read(n)
        if self._link.available() >= n:
            return self._link.drain_until(n, cutoff(None, deadline))
        stop = await self._wait(self._link.predicate_available(n), for_, deadline)
        return self._link.drain_until(n, cutoff(stop, deadline))

    async def read_packets(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[CdcPacket]:
        """As `read`, `n` packets with their times. The rest of a packet a `read` split counts
        as one. A read that waits returns no packet with an `at` past the time its wait stopped.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no packet past it, even one queued at the call. Already past, the
        read ends at once with the packets ready by it.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
        """
        check_wait(for_, deadline)
        self._check_read(n)
        if self._link.packets() >= n:
            return _cdc_packets(self._link.drain_packets_until(n, cutoff(None, deadline)))
        stop = await self._wait(self._link.predicate_packets(n), for_, deadline)
        return _cdc_packets(self._link.drain_packets_until(n, cutoff(stop, deadline)))

    async def read_until(
        self,
        pattern: re.Pattern[bytes],
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
        max_bytes: int = 65536,
    ) -> tuple[bytes, re.Match[bytes]] | None:
        """Run the machine until the bytes the device sent hold a match of `pattern`, then take
        every byte up to the match end, as `UartStream.read_until`, with its shortest-prefix rule.
        Packets count as their bytes, a match may span them, and a packet holding several bytes
        gives the same match as the bytes one by one.

        Returns:
            The bytes through `match.end()`, and the match over the searched copy. None when
            `for_` elapsed or `deadline` passed first, the core went idle, or `max_bytes` bytes
            are queued without a match: nothing is taken.

        Raises:
            ValueError: `pattern` matches the empty string, or `max_bytes` is not above 0.
            TypeError: `pattern` is no compiled bytes regex, both `for_` and `deadline` given,
                or `deadline` is no `VirtualTime`.
            RuntimeError: The function is bridged to a pty, or, while this one waited, another
                task took queued bytes or the queue dropped bytes at its limit.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        self._check_read(0)
        link = self._link
        return await read_until_match(
            self._wait,
            pattern,
            link,
            link.drain,
            for_=for_,
            deadline=deadline,
            max_bytes=max_bytes,
        )

    def take(self) -> bytes:
        """Every byte the device sent and not read yet, at once, never running the machine.
        Allowed parked and running, from any thread. Bytes arriving during the call stay
        queued for the next one. A take while a `read` of the same stream waits can leave that
        read short.

        Raises:
            RuntimeError: The function is bridged to a pty.
        """
        self._check_read(0)
        link = self._link
        return link.drain(link.available())

    def take_packets(self) -> list[CdcPacket]:
        """As `take`, the packets with their times. The rest of a packet a `read` split counts
        as one. A take while a `read_packets` of the same stream waits can leave that read
        short.

        Raises:
            RuntimeError: The function is bridged to a pty.
        """
        self._check_read(0)
        link = self._link
        return _cdc_packets(link.drain_packets(link.packets()))

    async def wait_ready(
        self,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> None:
        """Run the machine until `ready`, or `for_` elapsed.

        `UsbHost.wait_configured` returns before the driver's requests end, 3 ms before on the
        TinyUSB 0.21.0 example. Inside `running()`, this awaits the background run instead of
        driving the machine.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`. Already
        past, the wait ends at once.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed,
                `deadline` passed or the core went idle. Its status is `unbound` while no
                driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if not self.ready:
            await self._wait(self._link.predicate_ready(), for_, deadline)
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
        transfer, at = await self._post(link.command_set_control_line_state(dtr, rts))
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
            link.command_set_line_coding(baud, stop_code, parity_code, data_bits)
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
        status, done_at = await await_result(
            self._link,
            self._pending,
            self._wait,
            transfer,
            posted_at,
            f"{where}: the run ended before {label} did",
        )
        if status is _core.UsbTransferStatus.completed:
            return done_at
        message = f"{where}: {label} ended {status.name}"
        if status is _core.UsbTransferStatus.stalled:
            raise UsbStall(message, status=status.name)
        if status is _core.UsbTransferStatus.timed_out:
            raise UsbTimeout(message, status=status.name)
        raise UsbError(message, status=status.name)

    @override
    def __repr__(self) -> str:
        return f"CdcStream({self.name}, {self.function})"


def _cdc_packets(records: Iterable[_core.CdcPacketRecord]) -> list[CdcPacket]:
    return [CdcPacket(bytes(record.data), VirtualTime(record.at_ps)) for record in records]
