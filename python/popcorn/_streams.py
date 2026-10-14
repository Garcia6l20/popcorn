"""UART byte streams in virtual time, and host ptys bridged to UARTs and USB CDC ACM
functions."""

from __future__ import annotations

import re
from collections.abc import Awaitable, Callable, Iterable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Protocol, cast

from popcorn import _core
from popcorn._devices import BusDevice, I2CTarget, SpiController, SpiTarget
from popcorn._enums import UartError
from popcorn._errors import ConfigError
from popcorn.devices import At25sf, Eeprom24xx, TableTarget
from popcorn.units import VirtualDuration, VirtualTime

_MAX_PS = (1 << 64) - 1

if TYPE_CHECKING:
    from popcorn._refs import Pending
    from popcorn._usb import UsbHosts


@dataclass(frozen=True, slots=True)
class Gated:
    """A Python predicate called only at the boundaries where a native gate holds: a boundary
    where the gate does not hold takes no GIL."""

    gate: _core.Predicate
    ready: Callable[[], bool]


type Waiter = Callable[
    [_core.Predicate | Gated, VirtualDuration | None, VirtualTime | None],
    Awaitable[VirtualTime],
]
"""Runs the machine until a predicate holds, a virtual duration elapses or a deadline passes,
and returns the virtual time it stopped waiting at. A native predicate runs on the machine's
thread with no GIL, the Python predicate of a `Gated` on that thread holding the GIL, as
`Machine.wait_until`'s, where its gate holds."""


def check_wait(for_: VirtualDuration | None, deadline: object) -> None:
    """Refuse a wait given both a duration and a deadline, or a deadline that is no time.

    Raises:
        TypeError: Both are given, or `deadline` is no `VirtualTime`.
    """
    if for_ is not None and deadline is not None:
        raise TypeError("for_ and deadline are exclusive")
    if deadline is not None and not isinstance(deadline, VirtualTime):
        raise TypeError("deadline is a VirtualTime, a point in time: for a duration use for_")


class _Matcher:
    """What a `read_until` wait checks on the machine's thread: the queued bytes copied so far,
    searched over each new prefix as they grow, so the first match is the one over the shortest
    prefix that holds one, wherever the boundaries fell. `mark` holds the count already copied,
    the gate of the wait."""

    __slots__ = (
        "_available",
        "_max",
        "_pattern",
        "_peek",
        "found",
        "full",
        "mark",
        "shrunk",
        "snapshot",
    )

    def __init__(
        self,
        pattern: re.Pattern[bytes],
        peek: Callable[[int, int], bytes],
        available: Callable[[], int],
        max_bytes: int,
    ) -> None:
        self._pattern = pattern
        self._peek = peek
        self._available = available
        self._max = max_bytes
        self.mark = _core.CountMark()
        self.snapshot = b""
        self.found: re.Match[bytes] | None = None
        self.full = False
        self.shrunk = False

    def __call__(self) -> bool:
        if self.found is not None or self.full or self.shrunk:
            return True
        if self._available() < len(self.snapshot):
            self.shrunk = True
            return True
        searched = len(self.snapshot)
        self.snapshot += self._peek(searched, self._max - searched)
        self.mark.set(len(self.snapshot))
        for end in range(searched + 1, len(self.snapshot) + 1):
            self.found = self._pattern.search(self.snapshot, 0, end)
            if self.found is not None:
                return True
        self.full = len(self.snapshot) >= self._max
        return self.full


class QueueLink(Protocol):
    """The queue of bytes a `read_until` searches: a UART or a CDC link."""

    def peek(self, start: int, count: int, /) -> bytes:
        """Copy at most `count` queued bytes from `start`, taking none."""
        ...

    def available(self) -> int:
        """Queued bytes."""
        ...

    def dropped(self) -> int:
        """Bytes dropped at the queue's limit."""
        ...

    def predicate_changed(self, mark: _core.CountMark, /) -> _core.Predicate:
        """A predicate true while `available()` differs from `mark`."""
        ...


async def read_until_match(
    wait: Waiter,
    pattern: re.Pattern[bytes],
    link: QueueLink,
    take: Callable[[int], bytes],
    *,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None,
    max_bytes: int,
) -> tuple[bytes, re.Match[bytes]] | None:
    """The body of `UartStream.read_until` and `CdcStream.read_until`, over a stream's queue.

    Raises:
        ValueError: `pattern` matches the empty string, or `max_bytes` is not above 0.
        TypeError: `pattern` is no compiled bytes regex, both `for_` and `deadline` given, or
            `deadline` is no `VirtualTime`.
        RuntimeError: The queue dropped bytes, or another task took queued bytes, while this
            one waited.
    """
    source: object = getattr(pattern, "pattern", None)
    if not isinstance(cast(object, pattern), re.Pattern) or not isinstance(source, bytes):
        raise TypeError(f"pattern is a compiled bytes regex, re.compile(rb'...'), not {pattern!r}")
    if pattern.search(b"") is not None:
        raise ValueError(f"{pattern!r} matches no bytes, it would take nothing")
    check_wait(for_, deadline)
    if max_bytes <= 0:
        raise ValueError(f"max_bytes must be above 0, not {max_bytes}")
    matcher = _Matcher(pattern, link.peek, link.available, max_bytes)
    dropped = link.dropped()
    await wait(Gated(link.predicate_changed(matcher.mark), matcher), for_, deadline)
    if link.dropped() != dropped:
        raise RuntimeError("the queue dropped bytes at its limit while read_until waited")
    if matcher.shrunk:
        raise RuntimeError("another reader took queued bytes while read_until waited")
    found = matcher.found
    if found is None:
        return None
    taken = take(found.end())
    if taken != matcher.snapshot[: found.end()]:
        raise RuntimeError("another reader took queued bytes while read_until waited")
    return taken, found


def deadline_ps(deadline: VirtualTime | None) -> int | None:
    """The deadline in picoseconds since reset, a negative one as 0, past, and one past 64 bits
    as the largest, never."""
    return None if deadline is None else min(max(0, deadline.ps), 2**64 - 1)


def cutoff(stop: VirtualTime | None, deadline: VirtualTime | None) -> int:
    """The latest ready time a stream read takes, in picoseconds: the time its wait stopped,
    or none without a wait, capped at `deadline`."""
    bound = 2**64 - 1 if stop is None else stop.ps
    return bound if deadline is None else min(bound, max(0, deadline.ps))


class Poster(Protocol):
    """Runs a native command as one command on the machine, with no GIL: at once while it is
    parked, at a block boundary on its thread while it runs. The result is the command's value
    and the virtual time it ran at. None runs nothing and returns only the time."""

    def __call__(
        self,
        command: _core.AccessOp | None,
        at: VirtualTime | None = None,
        *,
        cancel: bool = True,
        follow: Callable[[tuple[Any, VirtualTime], _core.HostFuture], Any] | None = None,
        dropped: Callable[[_core.HostFuture], None] | None = None,
    ) -> Pending[tuple[Any, VirtualTime]]:
        """Post `command`."""
        ...


def written(posted: Pending[tuple[None, VirtualTime]]) -> Pending[VirtualTime]:
    """The time a posted stream write executed at, as its `Pending` value."""
    return posted.mapped(lambda done: done[1])


@dataclass(frozen=True, slots=True)
class Frame:
    """One frame the firmware transmitted.

    Attributes:
        data: Data bits, 0 for a break. With a parity, CR1.PCE, the parity bit the frame
            carried on the line in the MSB of its M-bit word is stripped: a 7E1 frame gives
            7 bits, an 8E1 frame 8 bits, M = 7 with PCE 6 bits. `read` gives the same bits.
        at: Virtual time its stop bit ended.
        brk: Whether it was a break. `read` gives a break as a 0 byte.
    """

    data: int
    at: VirtualTime
    brk: bool = False


class UartStream:
    """The host end of a UART, `m.periph.USART1.port`: bytes in and out in virtual time.

    Bytes written enter the firmware's receiver one frame time apart at the baud the firmware
    configured. Bytes the firmware transmits are queued with the time their stop bit ended, no
    Python call per byte. Deterministic: the same writes at the same virtual times give the same
    frames at the same times.

    Capture starts at power-on or at the last `Machine.restore`: every built-in UART is
    connected when the machine is built, so frames sent before the test first takes `port` wait
    in the queue. An alias, such as a secure address window of the same USART, shares the
    stream of the peripheral serving it. The queue grows by one frame per transmitted character
    until read, about 16 bytes each, up to 1 Mi frames, about 16 MiB. A full queue drops its
    oldest frame and counts it in `dropped`.

    `Machine.restore` empties the queue of transmitted frames not read yet, since they came
    from a run the restore undoes. Bytes written and not yet delivered are part of the UART
    model's state and return to what the snapshot holds.
    """

    __slots__ = (
        "__weakref__",
        "_bridges",
        "_dropped",
        "_link",
        "_linked",
        "_post",
        "_wait",
        "name",
    )

    def __init__(
        self,
        name: str,
        link: _core.UartQueue,
        wait: Waiter,
        bridges: dict[str, PtyBridge],
        post: Poster,
        dropped: Callable[[], int],
        linked: set[str] | None = None,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.port` instead."""
        self.name = name
        self._dropped = dropped
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._post = post
        self._linked = set[str]() if linked is None else linked

    @property
    def available(self) -> int:
        """Frames the firmware transmitted, not read yet."""
        return self._link.available()

    @property
    def dropped(self) -> int:
        """Transmitted frames dropped because the queue held its limit, since power-on or the
        last `Machine.restore`."""
        return self._dropped()

    @property
    def queued(self) -> int:
        """Bytes and breaks written and not yet delivered to the firmware's receiver.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: use
                `m.read(m.ref.periph.<NAME>.port.queued)`.
        """
        return self._link.queued()

    def write(
        self, data: bytes, *, at: VirtualTime | None = None, error: UartError | None = None
    ) -> Pending[VirtualTime]:
        """Queue bytes towards the firmware, posted at the call as one command: at once while the
        machine is parked, at the next block boundary while it runs, or at the first boundary at
        or after `at`. They arrive one frame time apart as the machine runs. Await the result
        for the virtual time the bytes were queued at. With a parity configured, each byte
        arrives with the parity bit the receiver expects in the MSB of its M-bit word, so bit 7
        of a byte sent to a 7E1 receiver is replaced, and none sets PE unless tagged. With
        CR3.RTSE the bytes also wait for the firmware's RTS, see `set_cts`.

        `error` tags every byte of this write: `UartError.FRAMING` sets ISR.FE and `NOISE`
        ISR.NE with the byte's data, `PARITY` inverts its parity bit so ISR.PE rises, or logs
        once and delivers the byte clean with CR1.PCE 0. The flag rises when the byte reaches
        RDR, in FIFO mode behind the bytes ahead of it. A byte finding the RX FIFO full is
        dropped with its tag and sets ORE. Write the bytes that need a tag separately.

        Raises:
            TypeError: `error` is neither a `UartError` nor None, at the call.
            RuntimeError: Called from a handler or a model, or the UART is linked to another
                member of a `System`: its receiver hears the link only, inject through
                `System.tap`.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        tag = cast(object, error)
        if not isinstance(tag, UartError | None):
            raise TypeError(f"error must be a UartError or None, not {type(tag).__name__}")
        self._check_writable()
        link = self._link
        data = bytes(data)
        value = 0 if error is None else error.value
        return written(self._post(link.command_send(data, value), at))

    def write_break(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue one break towards the firmware after the bytes already queued, posted as `write`
        is. It reads as a 0 byte with ISR.FE, as a break from a linked machine does, and holds
        the line one frame time plus 2 bit times before the next byte. Await the result for the
        virtual time it was queued at.

        Raises:
            RuntimeError: Called from a handler or a model, or the UART is linked to another
                member of a `System`, as for `write`.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        self._check_writable()
        return written(self._post(self._link.command_break(), at))

    def set_cts(self, ready: bool, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Drive the firmware's nCTS input to `ready` at exactly `at`, or when the command runs
        without `at`. `ready` True is nCTS low, clear to send, the level at power-on. Posted as
        one command as `Pin.drive_edges` is, at once while the machine is parked, at the next
        block boundary while it runs, and applied through a UART model event at its own time,
        whatever the boundary the command ran at. A change at the time the command runs applies
        at the next event dispatch, before the next instruction.

        With CR3.CTSE 1 the firmware's transmitter starts no frame while the input is False,
        completes the frame in flight, and starts a held frame at the time of the change to
        True. ISR.TC stays 0 while a frame is held. A frame starting at the very instant of a
        change to False is not held, whatever the order of the events of that instant, as on
        the chip, where CTS must change 3 kernel clocks before a frame ends to hold the next.
        With CTSE each change sets ISR.CTSIF, and ISR.CTS reads the level. The level and the
        changes still pending are board state: a snapshot saves them, and an MCU reset of any
        kind keeps the level and each pending change at its instant, so a change due at
        `m.time` T before a reset at R applies at `m.time` T - R after it. A `System` refuses to
        link the UART while the level is False or a change is pending, since the link offers no
        way to release it. Await the result for the time the command ran at.

        The stream also honours the firmware's RTS without being asked: with CR3.RTSE 1 and
        CR3.DEM 0, a written byte waits before its start bit while RDR is full, or the RX FIFO in
        FIFO mode, and starts at the read or flush that frees space. Such a firmware never
        overruns on written bytes. See `rts_ready`.

        Raises:
            TypeError: `ready` is no bool or `at` no `VirtualTime`, at the call.
            ValueError: `at` is outside 0 to 2**64 - 1 ps, at the call. From the await, `at` is
                already past when the command runs.
            ConfigError: From the await, the UART would keep more than 65536 pending changes.
            RuntimeError: Called from a handler or a model, or the UART is linked to another
                member of a `System`: the link carries no flow control.
        """
        level = cast(object, ready)
        if not isinstance(level, bool):
            raise TypeError(f"ready must be a bool, not {type(level).__name__}")
        when = cast(object, at)
        if not isinstance(when, VirtualTime | None):
            raise TypeError(f"at must be a VirtualTime or None, not {type(when).__name__}")
        if at is not None and not 0 <= at.ps <= _MAX_PS:
            raise ValueError(f"a CTS change at {at} leaves the 64-bit picosecond range")
        if self.name in self._linked:
            raise RuntimeError(
                f"{self.name} is linked in a System: the link carries no flow control"
            )
        command = self._link.command_set_cts(level, None if at is None else at.ps)
        return written(self._post(command))

    @property
    def rts_ready(self) -> bool:
        """Whether the firmware's RTS output says ready to receive. False only with CR3.RTSE 1,
        CR3.DEM 0 and RDR full, or the RX FIFO full in FIFO mode. While it is False, bytes from
        `write` wait before their start bit.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks.
        """
        return self._link.rts_ready()

    def _check_writable(self) -> None:
        if self.name in self._linked:
            raise RuntimeError(
                f"{self.name} is linked in a System: inject through the system's tap"
            )

    async def read(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine. The
        run stops as `Machine.wait_for` does: after the last frame's own time, at the first event
        boundary or slice end, or past `for_` by the bound `Machine.run` gives for its `for_`.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no frame past it, even one queued at the call. Already past, the
        read ends at once with the frames ready by it.

        A read that waits takes only the frames whose stop bit ended by the time its wait
        stopped, in one native step: frames that end later stay queued for the next read, even
        those a free run queued before this read resumed. So with `deadline` or neither, the
        bytes depend only on what the firmware sent, the same parked and running. Inside a free
        run `for_` counts from the boundary where the wait registered, which wall time sets.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed, `deadline` passed or the core went
                idle first.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            RuntimeError: The UART is bridged to a pty, which takes what the firmware sends.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        return bytes(f.data & 0xFF for f in await self.read_frames(n, for_=for_, deadline=deadline))

    async def read_frames(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[Frame]:
        """As `read`, the frames with their times. A read that waits returns no frame with an
        `at` past the time its wait stopped.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no frame past it, even one queued at the call. Already past, the
        read ends at once with the frames ready by it.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
        """
        check_wait(for_, deadline)
        self._check_unbridged()
        if n < 0:
            raise ValueError(f"cannot read {n} frames")
        if self._link.available() >= n:
            return _frames(self._link.drain_until(n, cutoff(None, deadline)))
        stop = await self._wait(self._link.predicate_available(n), for_, deadline)
        return _frames(self._link.drain_until(n, cutoff(stop, deadline)))

    async def read_until(
        self,
        pattern: re.Pattern[bytes],
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
        max_bytes: int = 65536,
    ) -> tuple[bytes, re.Match[bytes]] | None:
        """Run the machine until the queued bytes hold a match of `pattern`, then take every
        byte up to the match end. The bytes after it stay queued.

            found = await port.read_until(re.compile(rb"ready\\r?\\n"), for_=ms(50))

        The match is the one over the shortest prefix of the bytes that holds one, so a growing
        pattern such as `rb"\\d+"` stops at its first digit: end it with what must follow,
        `rb"\\d+\\r?\\n"`. The bytes and the match depend only on the bytes received, the same
        parked and running. The time the wait ends does not: parked it is the event boundary
        after the byte that completed the match, inside a free run the slice end after it.

        The search runs on the machine's thread holding the GIL, at the event boundaries and
        slice ends where the queued byte count changed, inside a free run at slice ends only,
        over a copy of the first `max_bytes` queued bytes. A boundary where the count did not
        change takes no GIL, nor does the end of the wait, except a wait cancelled inside a free
        run, which can release the pattern on the machine's thread, one GIL entry. Each new
        byte costs one `pattern.search` over the bytes before it, so a burst without a match
        costs time quadratic in its length, up to `max_bytes`.
        A slow pattern stalls the machine as a slow handler does. `deadline` stops waiting at
        this virtual time instead, exclusive with `for_`. Already past, the wait ends at once.

        Returns:
            The bytes through `match.end()`, and the match over the searched copy. None when
            `for_` elapsed or `deadline` passed first, the core went idle, or `max_bytes` bytes
            are queued without a match: nothing is taken, `take()` returns what arrived.

        Raises:
            ValueError: `pattern` matches the empty string, which would take nothing, or
                `max_bytes` is not above 0.
            TypeError: `pattern` is no compiled bytes regex, both `for_` and `deadline` given,
                or `deadline` is no `VirtualTime`.
            RuntimeError: The UART is bridged to a pty, or, while this one waited, another task
                took queued bytes or the queue dropped bytes at its limit.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        self._check_unbridged()
        link = self._link
        return await read_until_match(
            self._wait,
            pattern,
            link,
            lambda n: bytes(f.data & 0xFF for f in link.drain(n)),
            for_=for_,
            deadline=deadline,
            max_bytes=max_bytes,
        )

    def take(self) -> bytes:
        """Every byte the firmware transmitted and not read yet, at once, never running the
        machine. Allowed parked and running, from any thread. A take while a `read` of the same
        stream waits can leave that read short.

        Raises:
            RuntimeError: The UART is bridged to a pty.
        """
        return bytes(f.data & 0xFF for f in self.take_frames())

    def take_frames(self) -> list[Frame]:
        """As `take`, the frames with their times. A frame arriving during the call stays
        queued for the next one. A take while a `read` of the same stream waits can leave that
        read short.

        Raises:
            RuntimeError: The UART is bridged to a pty.
        """
        self._check_unbridged()
        link = self._link
        return _frames(link.drain(link.available()))

    def _check_unbridged(self) -> None:
        bridge = self._bridges.get(self.name)
        if bridge is not None:
            raise RuntimeError(f"{self.name} is bridged to {bridge.path}")

    def __repr__(self) -> str:
        return f"UartStream({self.name})"


def _frames(records: Iterable[_core.UartFrame]) -> list[Frame]:
    return [Frame(f.data, VirtualTime(f.at_ps), f.brk) for f in records]


type PtyOpener = Callable[[str, _core.UartQueue | _core.CdcLink], _core.PtyBridge]
"""Bridges a UART or CDC link to a new host pty keyed by the name, pumped by the machine."""


class PtyBridge:
    """A UART or a USB CDC ACM function bridged to a host pty: host tools open `path` like a
    serial port.

    The machine's thread moves the bytes around every slice, and wakes an idle free run when
    host bytes arrive: a sync test body that blocks on the pty keeps the machine running. Host
    bytes enter the firmware at the next slice boundary, stamped at the virtual time of that
    boundary: a pty is not deterministic. A parked machine leaves host input unread in the pty
    until it runs again.

    Host input not yet delivered to a UART is kept across a restore. Bytes already in the
    UART's receiver return to the snapshot's state. Host input not yet taken by a USB device
    returns to what the snapshot held. Opening or closing the pty sends nothing to the device:
    a CDC function keeps the DTR its host set at configuration.

    Bytes for the host wait in a buffer while no tool reads the pty. The buffer keeps the last
    64 KiB: older bytes are dropped, with one warning naming the pty on the `popcorn` logger,
    through the machine's log. `Machine.restore` empties it.
    """

    __slots__ = ("__weakref__", "_native", "name", "path")

    def __init__(self, name: str, native: _core.PtyBridge) -> None:
        """Wrap a native bridge. Use `m.board.pty(name)` instead."""
        self.name = name
        self._native = native
        self.path = native.path()

    def close(self) -> None:
        """Stop pumping and close the pty."""
        self._native.close()

    def __repr__(self) -> str:
        return f"PtyBridge({self.name}, {self.path})"


class UartPorts:
    """The Python ends of a machine's UARTs, one link and one stream per UART."""

    __slots__ = (
        "__weakref__",
        "_aliases",
        "_connect",
        "_dropped_base",
        "_links",
        "_open_pty",
        "_post",
        "_streams",
        "_wait",
        "bridges",
        "linked",
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UartQueue],
        uarts: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        post: Poster,
        open_pty: PtyOpener,
    ) -> None:
        """Connect every UART named in `uarts` through `connect` at once, so each one queues
        what the firmware transmits from power-on. `aliases` maps an alias, such as a secure
        address window, to the peripheral serving it, which shares its link. Reads run the
        machine through `wait`, writes are posted through `post`, pty bridges are opened
        through `open_pty`."""
        self._aliases = dict(aliases)
        self._dropped_base: dict[str, int] = {}
        self._connect = connect
        self._wait = wait
        self._post = post
        self._open_pty = open_pty
        self._links: dict[str, _core.UartQueue] = {name: connect(name) for name in uarts}
        self._streams: dict[str, UartStream] = {}
        self.bridges: dict[str, PtyBridge] = {}
        self.linked: set[str] = set()

    def serving(self, name: str) -> str:
        """The peripheral that serves `name`, `name` itself unless it is an alias."""
        return self._aliases.get(name, name)

    def link(self, name: str) -> _core.UartQueue:
        """The native link of UART `name` or of the UART its alias names, connected at machine
        build for a built-in UART."""
        name = self.serving(name)
        link = self._links.get(name)
        if link is None:
            link = self._links[name] = self._connect(name)
        return link

    def stream(self, name: str) -> UartStream:
        """The stream of UART `name`, what `m.periph.<name>.port` is.

        Raises:
            ModelError: The peripheral's model is not a UART.
            ConfigError: The description has no such peripheral.
        """
        name = self.serving(name)
        stream = self._streams.get(name)
        if stream is None:
            link = self.link(name)
            stream = UartStream(
                name,
                link,
                self._wait,
                self.bridges,
                self._post,
                lambda: link.dropped() - self._dropped_base.get(name, 0),
                self.linked,
            )
            self._streams[name] = stream
        return stream

    def bridge(self, key: str, link: _core.UartQueue | _core.CdcLink) -> PtyBridge:
        """The bridge keyed `key`, bridging `link` to a new pty on first use."""
        found = self.bridges.get(key)
        if found is None:
            found = self.bridges[key] = PtyBridge(key, self._open_pty(key, link))
        return found

    def discard(self) -> None:
        """Drop transmitted frames not read yet, at a restore. The machine drops the bytes
        waiting for ptys itself."""
        for name, link in self._links.items():
            link.drain(link.available())
            self._dropped_base[name] = link.dropped()


class Board:
    """What is wired to the MCU: host ptys bridged to its UARTs and USB CDC functions, and the
    Python devices on its buses, `m.board`.

    Every built-in UART is connected to Python from power-on: its `port` holds what the
    firmware sent since power-on or the last `Machine.restore`, unless a pty bridge takes it.
    """

    __slots__ = ("_devices", "_part", "_peripherals", "_ports", "_usb")

    def __init__(
        self,
        ports: UartPorts,
        devices: Mapping[str, tuple[BusDevice, ...]],
        usb: UsbHosts | None = None,
        *,
        part: str,
        peripherals: Iterable[str],
    ) -> None:
        """Bridge the UARTs of `ports` and the CDC functions of the hosts of `usb`, list
        `devices` on the `peripherals` of `part`. `Machine.board` builds it."""
        self._ports = ports
        self._devices = dict(devices)
        self._usb = usb
        self._part = part
        self._peripherals = frozenset(peripherals)

    def devices(
        self, peripheral: str
    ) -> tuple[I2CTarget | SpiTarget | SpiController | At25sf | Eeprom24xx | TableTarget, ...]:
        """The devices on bus `peripheral`, in attachment order, empty for none. An alias, such
        as a secure address window, names the bus it serves."""
        return cast(
            "tuple[I2CTarget | SpiTarget | SpiController | At25sf | Eeprom24xx | TableTarget, ...]",
            self._devices.get(self._ports.serving(peripheral), ()),
        )

    def device[D: I2CTarget | SpiTarget | SpiController | At25sf | Eeprom24xx | TableTarget](
        self, peripheral: str, cls: type[D]
    ) -> D:
        """The one device of class `cls` on bus `peripheral`, typed. A subclass instance
        matches. An alias, such as a secure address window, names the bus it serves.

            codec = m.board.device("I2C2", Codec)

        Raises:
            ConfigError: `peripheral` is no peripheral of the part, or no device or several
                devices of `cls` are on it, key `board.<peripheral>`.
        """
        bus = self._ports.serving(peripheral)
        if bus not in self._peripherals:
            expected = f"a peripheral of {self._part}"
        else:
            found = [d for d in self.devices(bus) if isinstance(d, cls)]
            if len(found) == 1:
                return found[0]
            expected = f"one {cls.__name__} on {bus}, not {len(found)}"
        raise ConfigError(
            f"board.{peripheral}: {expected}",
            file=self._part,
            key=f"board.{peripheral}",
            expected=expected,
        )

    def pty(self, name: str, *, function: int = 0) -> str:
        """Bridge UART `name`, or a CDC ACM function of the virtual USB host of controller
        `name`, to a new host pty and return its path, the same one on later calls.

        The pty owns what the firmware sends, for the life of the machine: reads of the UART's
        `port`, or of the CDC function's `host.cdc()` stream, raise
        `RuntimeError("<key> is bridged to <path>")`, the key as in `ptys`.
        `Periph(connect="pty")` in a `MachineDescription` calls this at build for every machine of
        the
        machine description. Call it from the test that wants the pty instead, to keep stream reads
        in the others. A CDC function's pty is keyed `name` in `ptys` for function 0,
        `name.<function>` for the others.

        A free run advances while the caller blocks: a sync call, a thread join, or a C call that
        releases the GIL. Open a peer whose connection blocks wall time, a probe or a handshake
        on the pty, while the machine is parked, before `start()` or `running()`.

        Args:
            name: UART instance, or USB device controller with a virtual host.
            function: The n-th CDC ACM function of the device, in interface order. 0 for a
                UART.

        Raises:
            ValueError: `function` is negative, above 0 for a UART, or beyond the CDC ACM
                functions of a configuration already read.
            RuntimeError: The UART is linked by a `System`, until its `close()`.
            ModelError: The peripheral's model is neither a UART nor a USB device controller
                with a virtual host.
            ConfigError: The description has no such peripheral.
        """
        if self._usb is not None and self._usb.serves(name):
            link = self._usb.cdc_link(name, function)
            return self._ports.bridge(cdc_key(self._usb.host(name).name, function), link).path
        if function != 0:
            raise ValueError(f"{name}: a UART has no function {function}, only 0")
        name = self._ports.serving(name)
        if name in self._ports.linked:
            raise RuntimeError(f"{name} is linked in a System, which owns its line")
        return self._ports.bridge(name, self._ports.link(name)).path

    @property
    def ptys(self) -> dict[str, str]:
        """Pty path of each bridged UART and CDC function."""
        return {name: bridge.path for name, bridge in self._ports.bridges.items()}

    def close(self) -> None:
        """Close every pty."""
        for bridge in self._ports.bridges.values():
            bridge.close()
        self._ports.bridges.clear()


def cdc_key(name: str, function: int) -> str:
    """The key of CDC ACM function `function` of controller `name` in `Board.ptys`: `name` for
    function 0, `<name>.<function>` for the others."""
    return name if function == 0 else f"{name}.{function}"
