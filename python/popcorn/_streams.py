"""UART byte streams in virtual time, and their bridge to a host pty."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import time
import tty
import weakref
from collections.abc import Awaitable, Callable, Iterable, Mapping
from dataclasses import dataclass
from typing import cast

from popcorn import _core
from popcorn._devices import BusDevice, I2CTarget, SpiTarget
from popcorn.devices import At25sf, Eeprom24xx, TableTarget
from popcorn.units import VirtualDuration, VirtualTime

_LOG = logging.getLogger("popcorn")

OUTGOING_LIMIT = 64 * 1024
"""Bytes a pty keeps for the host while no tool reads it."""

type Waiter = Callable[[Callable[[], bool], VirtualDuration | None], Awaitable[None]]
"""Runs the machine until a predicate holds or a virtual duration elapses."""


@dataclass(frozen=True, slots=True)
class Frame:
    """One frame the firmware transmitted.

    Attributes:
        data: Data bits.
        at: Virtual time its stop bit ended.
    """

    data: int
    at: VirtualTime


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
    until read, about 16 bytes each.

    `Machine.restore` empties the queue of transmitted frames not read yet, since they came
    from a run the restore undoes. Bytes written and not yet delivered are part of the UART
    model's state and return to what the snapshot holds.
    """

    __slots__ = ("__weakref__", "_bridges", "_link", "_stimulus", "_wait", "name")

    def __init__(
        self,
        name: str,
        link: _core.UartLink,
        wait: Waiter,
        bridges: dict[str, PtyBridge],
        stimulus: Callable[[], None],
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.port` instead."""
        self.name = name
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._stimulus = stimulus

    @property
    def available(self) -> int:
        """Frames the firmware transmitted, not read yet."""
        return self._link.available()

    @property
    def queued(self) -> int:
        """Bytes written and not yet delivered to the firmware's receiver."""
        return self._link.queued()

    async def write(self, data: bytes) -> None:
        """Queue bytes towards the firmware. They arrive as the machine runs.

        Raises:
            RuntimeError: Called from another thread while a run is active.
        """
        self._stimulus()
        self._link.send(bytes(data))

    async def read(self, n: int, *, for_: VirtualDuration | None = None) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed or the core went idle first.

        Raises:
            RuntimeError: The UART is bridged to a pty, which takes what the firmware sends.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        return bytes(f.data & 0xFF for f in await self.read_frames(n, for_=for_))

    async def read_frames(self, n: int, *, for_: VirtualDuration | None = None) -> list[Frame]:
        """As `read`, the frames with their times."""
        bridge = self._bridges.get(self.name)
        if bridge is not None:
            raise RuntimeError(f"{self.name} is bridged to {bridge.path}")
        if n < 0:
            raise ValueError(f"cannot read {n} frames")
        if self._link.available() < n:
            await self._wait(lambda: self._link.available() >= n, for_)
        return [Frame(f.data, VirtualTime(f.at_ps)) for f in self._link.drain(n)]

    def __repr__(self) -> str:
        return f"UartStream({self.name})"


class PtyBridge:
    """A UART bridged to a host pty: host tools open `path` like a serial port.

    Host bytes enter the firmware at the next slice boundary, stamped at the virtual time of
    that boundary: a pty is not deterministic. Bytes the firmware transmits reach the pty at
    slice ends. When host bytes wait and no slice runs for `deadlock_warning` seconds, a warning
    on the `popcorn` logger names the pty: the remedy is to run the machine, typically in
    `async with m.running()`.

    Host input not yet delivered to the firmware is kept across a restore. Bytes already in
    the UART's receiver return to the snapshot's state.

    Bytes for the host wait in a buffer while no tool reads the pty. The buffer keeps the last
    64 KiB: older bytes are dropped, with one warning naming the pty. `Machine.restore` empties
    it.
    """

    __slots__ = (
        "__weakref__",
        "_finalizer",
        "_incoming",
        "_link",
        "_master",
        "_outgoing",
        "_overflowed",
        "_slave",
        "_waiting_since",
        "_watch",
        "deadlock_warning",
        "name",
        "path",
    )

    def __init__(self, name: str, link: _core.UartLink, deadlock_warning: float) -> None:
        """Open a pty for UART `name`. Use `m.board.pty(name)` instead."""
        master, slave = os.openpty()
        tty.setraw(slave)
        os.set_blocking(master, False)
        self._master = master
        self._slave = slave
        self.path = os.ttyname(slave)
        self.name = name
        self._link = link
        self.deadlock_warning = deadlock_warning
        self._incoming = bytearray()
        self._outgoing = bytearray()
        self._watch: list[asyncio.AbstractEventLoop | None] = [None]
        self._waiting_since: float | None = None
        self._overflowed = False
        self._finalizer = weakref.finalize(self, _close, self._watch, master, slave)
        self.attach_loop()

    def attach_loop(self) -> None:
        """Watch the pty from the running event loop, once per loop."""
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return
        if loop is self._watch[0]:
            return
        _unwatch(self._watch, self._master)
        self._watch[0] = loop
        loop.add_reader(self._master, _readable, weakref.ref[PtyBridge](self))

    def pump(self) -> None:
        """At a slice boundary: host bytes to the firmware, firmware bytes to the host."""
        self.attach_loop()
        if self._incoming:
            self._link.send(bytes(self._incoming))
            self._incoming.clear()
        self._waiting_since = None
        frames = self._link.drain(self._link.available())
        self._outgoing += bytes(f.data & 0xFF for f in frames)
        if len(self._outgoing) > OUTGOING_LIMIT:
            del self._outgoing[: len(self._outgoing) - OUTGOING_LIMIT]
            if not self._overflowed:
                self._overflowed = True
                _LOG.warning(
                    "%s: no host tool reads %s, bytes beyond the last %d were dropped",
                    self.name,
                    self.path,
                    OUTGOING_LIMIT,
                )
        if self._outgoing:
            with contextlib.suppress(BlockingIOError):
                written = os.write(self._master, self._outgoing)
                del self._outgoing[:written]

    def discard(self) -> None:
        """Drop the bytes waiting for the host, at a restore."""
        self._outgoing.clear()

    def close(self) -> None:
        """Stop watching and close the pty."""
        self._finalizer()

    def on_readable(self) -> None:
        """Take what the host wrote, from the loop's reader."""
        try:
            data = os.read(self._master, 4096)
        except BlockingIOError, OSError:
            return
        self._incoming += data
        loop = self._watch[0]
        if self._waiting_since is None and loop is not None:
            since = self._waiting_since = time.monotonic()
            loop.call_later(self.deadlock_warning, _deadlock, weakref.ref[PtyBridge](self), since)

    def check_deadlock(self, since: float) -> None:
        """Warn when host input still waits since `since`, from a loop timer."""
        if self._waiting_since != since:
            return
        _LOG.warning(
            "%s: host input on %s waited %.2f s with no slice run, run the machine, for "
            "instance inside `async with m.running()`",
            self.name,
            self.path,
            time.monotonic() - since,
        )

    def __repr__(self) -> str:
        return f"PtyBridge({self.name}, {self.path})"


class UartPorts:
    """The Python ends of a machine's UARTs, one link and one stream per UART."""

    __slots__ = ("_aliases", "_connect", "_links", "_stimulus", "_streams", "_wait", "bridges")

    def __init__(
        self,
        connect: Callable[[str], _core.UartLink],
        uarts: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        stimulus: Callable[[], None],
    ) -> None:
        """Connect every UART named in `uarts` through `connect` at once, so each one queues
        what the firmware transmits from power-on. `aliases` maps an alias, such as a secure
        address window, to the peripheral serving it, which shares its link. Reads run the
        machine through `wait`, writes are checked by `stimulus`."""
        self._aliases = dict(aliases)
        self._connect = connect
        self._wait = wait
        self._stimulus = stimulus
        self._links: dict[str, _core.UartLink] = {name: connect(name) for name in uarts}
        self._streams: dict[str, UartStream] = {}
        self.bridges: dict[str, PtyBridge] = {}

    def serving(self, name: str) -> str:
        """The peripheral that serves `name`, `name` itself unless it is an alias."""
        return self._aliases.get(name, name)

    def link(self, name: str) -> _core.UartLink:
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
            stream = UartStream(name, self.link(name), self._wait, self.bridges, self._stimulus)
            self._streams[name] = stream
        return stream

    def pump(self) -> None:
        """Move pty bytes at a slice boundary. The machine calls it."""
        for bridge in self.bridges.values():
            bridge.pump()

    def discard(self) -> None:
        """Drop transmitted frames not read yet and bytes waiting for ptys, at a restore."""
        for link in self._links.values():
            link.drain(link.available())
        for bridge in self.bridges.values():
            bridge.discard()


class Board:
    """What is wired to the MCU: host ptys bridged to its UARTs and the Python devices on its
    buses, `m.board`.

    Every built-in UART is connected to Python from power-on: its `port` holds what the
    firmware sent since power-on or the last `Machine.restore`, unless a pty bridge takes it.
    """

    __slots__ = ("_devices", "_ports")

    def __init__(
        self,
        ports: UartPorts,
        devices: Mapping[str, tuple[BusDevice, ...]],
    ) -> None:
        """Bridge the UARTs of `ports`, list `devices`. `Machine.board` builds it."""
        self._ports = ports
        self._devices = dict(devices)

    def devices(
        self, peripheral: str
    ) -> tuple[I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget, ...]:
        """The devices on bus `peripheral`, in attachment order, empty for none."""
        return cast(
            "tuple[I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget, ...]",
            self._devices.get(peripheral, ()),
        )

    def pty(self, name: str, *, deadlock_warning: float = 2.0) -> str:
        """Bridge UART `name` to a new host pty and return its path, the same one on later calls.

        A bridged UART is read by the pty only: its `port` refuses reads.

        Args:
            name: UART instance.
            deadlock_warning: Wall seconds host input may wait with no slice run before a
                warning names the pty.

        Raises:
            ModelError: The peripheral's model is not a UART.
            ConfigError: The description has no such peripheral.
        """
        bridges = self._ports.bridges
        name = self._ports.serving(name)
        bridge = bridges.get(name)
        if bridge is None:
            bridge = PtyBridge(name, self._ports.link(name), deadlock_warning)
            bridges[name] = bridge
        return bridge.path

    @property
    def ptys(self) -> dict[str, str]:
        """Pty path of each bridged UART."""
        return {name: bridge.path for name, bridge in self._ports.bridges.items()}

    def close(self) -> None:
        """Close every pty."""
        for bridge in self._ports.bridges.values():
            bridge.close()
        self._ports.bridges.clear()


def _readable(bridge: weakref.ref[PtyBridge]) -> None:
    alive = bridge()
    if alive is not None:
        alive.on_readable()


def _deadlock(bridge: weakref.ref[PtyBridge], since: float) -> None:
    alive = bridge()
    if alive is not None:
        alive.check_deadlock(since)


def _unwatch(watch: list[asyncio.AbstractEventLoop | None], fd: int) -> None:
    loop = watch[0]
    watch[0] = None
    if loop is not None and not loop.is_closed():
        loop.remove_reader(fd)


def _close(watch: list[asyncio.AbstractEventLoop | None], *fds: int) -> None:
    _unwatch(watch, fds[0])
    for fd in fds:
        with contextlib.suppress(OSError):
            os.close(fd)
