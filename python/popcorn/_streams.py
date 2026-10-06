"""UART byte streams in virtual time, and host ptys bridged to UARTs and USB CDC ACM
functions."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import threading
import time
import tty
import weakref
from collections.abc import Awaitable, Callable, Iterable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Protocol, cast

from popcorn import _core
from popcorn._devices import BusDevice, I2CTarget, SpiTarget
from popcorn.devices import At25sf, Eeprom24xx, TableTarget
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending
    from popcorn._usb import UsbHosts

_LOG = logging.getLogger("popcorn")

OUTGOING_LIMIT = 64 * 1024
"""Bytes a pty keeps for the host while no tool reads it."""

type Waiter = Callable[[Callable[[], bool], VirtualDuration | None], Awaitable[VirtualTime]]
"""Runs the machine until a predicate holds or a virtual duration elapses, and returns the
virtual time it stopped waiting at."""


class Poster(Protocol):
    """Runs a body as one command on the machine: at once while it is parked, at a block
    boundary on its thread while it runs. The result is the body's value and the virtual time
    it ran at."""

    def __call__[T](
        self,
        body: Callable[[], T],
        at: VirtualTime | None = None,
        *,
        cancel: bool = True,
        follow: Callable[[tuple[T, VirtualTime]], Any] | None = None,
    ) -> Pending[tuple[T, VirtualTime]]:
        """Post `body`."""
        ...


def written(posted: Pending[tuple[None, VirtualTime]]) -> Pending[VirtualTime]:
    """The time a posted stream write executed at, as its `Pending` value."""
    return posted.mapped(lambda done: done[1])


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
    until read, about 16 bytes each, up to 1 Mi frames, about 16 MiB. A full queue drops its
    oldest frame and counts it in `dropped`.

    `Machine.restore` empties the queue of transmitted frames not read yet, since they came
    from a run the restore undoes. Bytes written and not yet delivered are part of the UART
    model's state and return to what the snapshot holds.
    """

    __slots__ = ("__weakref__", "_bridges", "_dropped", "_link", "_post", "_wait", "name")

    def __init__(
        self,
        name: str,
        link: _core.UartLink,
        wait: Waiter,
        bridges: dict[str, PtyBridge],
        post: Poster,
        dropped: Callable[[], int],
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.port` instead."""
        self.name = name
        self._dropped = dropped
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._post = post

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
        """Bytes written and not yet delivered to the firmware's receiver.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: use
                `m.read(m.ref.periph.<NAME>.port.queued)`.
        """
        return self._link.queued()

    def write(self, data: bytes, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue bytes towards the firmware, posted at the call as one command: at once while the
        machine is parked, at the next block boundary while it runs, or at the first boundary at
        or after `at`. They arrive one frame time apart as the machine runs. Await the result
        for the virtual time the bytes were queued at.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link = self._link
        data = bytes(data)
        return written(self._post(lambda: link.send(data), at))

    async def read(self, n: int, *, for_: VirtualDuration | None = None) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine. The
        run stops as `Machine.wait_for` does: after the last frame's own time, at the first event
        boundary or slice end, or past `for_` by the bound `Machine.run` gives for its `for_`.

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


class PtyLink(Protocol):
    """The byte link a pty bridge moves bytes through."""

    def send(self, data: bytes, /) -> None:
        """Queue bytes towards the firmware."""
        ...

    def available(self) -> int:
        """Bytes the firmware sent and not taken yet."""
        ...

    def drain(self, count: int, /) -> bytes:
        """Take at most `count` of the oldest bytes the firmware sent."""
        ...


class UartPtyLink:
    """A UART link as a pty bridge takes it: the low 8 bits of each frame."""

    __slots__ = ("_link",)

    def __init__(self, link: _core.UartLink) -> None:
        """Wrap a native UART link."""
        self._link = link

    def send(self, data: bytes, /) -> None:
        """Queue bytes towards the firmware, one frame each."""
        self._link.send(data)

    def available(self) -> int:
        """Frames the firmware transmitted, not taken yet."""
        return self._link.available()

    def drain(self, count: int, /) -> bytes:
        """Take at most `count` of the oldest frames, their low 8 bits."""
        return bytes(f.data & 0xFF for f in self._link.drain(count))


class PtyBridge:
    """A UART or a USB CDC ACM function bridged to a host pty: host tools open `path` like a
    serial port.

    Host bytes enter the firmware at the next slice boundary, stamped at the virtual time of
    that boundary: a pty is not deterministic. Bytes the firmware transmits reach the pty at
    slice ends. When host bytes wait and no slice runs for `deadlock_warning` seconds, a warning
    on the `popcorn` logger names the pty: the remedy is to run the machine, typically in
    `async with m.running()`.

    Host input not yet delivered to a UART is kept across a restore. Bytes already in the
    UART's receiver return to the snapshot's state. Host input not yet taken by a USB device
    returns to what the snapshot held. Opening or closing the pty sends nothing to the device:
    a CDC function keeps the DTR its host set at configuration.

    Bytes for the host wait in a buffer while no tool reads the pty. The buffer keeps the last
    64 KiB: older bytes are dropped, with one warning naming the pty. `Machine.restore` empties
    it.
    """

    __slots__ = (
        "__weakref__",
        "_finalizer",
        "_incoming",
        "_link",
        "_lock",
        "_master",
        "_outgoing",
        "_overflowed",
        "_slave",
        "_waiting_since",
        "_wake",
        "_watch",
        "deadlock_warning",
        "name",
        "path",
    )

    def __init__(
        self,
        name: str,
        link: PtyLink,
        deadlock_warning: float,
        wake: Callable[[], None] = lambda: None,
    ) -> None:
        """Open a pty for `name` over `link`. `wake` makes a running machine take a slice.
        Use `m.board.pty(name)` instead."""
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
        self._lock = threading.Lock()
        self._wake = wake
        self._outgoing = bytearray()
        self._watch: list[asyncio.AbstractEventLoop | None] = [None]
        self._waiting_since: float | None = None
        self._overflowed = False
        self._finalizer = weakref.finalize(self, _close, self._watch, self._lock, master, slave)
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
        """At a slice boundary, on the machine's thread: host bytes to the firmware, firmware
        bytes to the host."""
        self.attach_loop()
        with self._lock:
            incoming, self._incoming = self._incoming, bytearray()
        if incoming:
            self._link.send(bytes(incoming))
        self._waiting_since = None
        self._outgoing += self._link.drain(self._link.available())
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
            with self._lock, contextlib.suppress(BlockingIOError):
                if self._finalizer.alive:
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
        with self._lock:
            self._incoming += data
        self._wake()
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

    __slots__ = (
        "__weakref__",
        "_aliases",
        "_bridged",
        "_connect",
        "_dropped_base",
        "_links",
        "_post",
        "_streams",
        "_wait",
        "bridges",
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UartLink],
        uarts: Iterable[str],
        aliases: Mapping[str, str],
        wait: Waiter,
        post: Poster,
        bridged: Callable[[], None] = lambda: None,
    ) -> None:
        """Connect every UART named in `uarts` through `connect` at once, so each one queues
        what the firmware transmits from power-on. `aliases` maps an alias, such as a secure
        address window, to the peripheral serving it, which shares its link. Reads run the
        machine through `wait`, writes are posted through `post`. `bridged` is called when a
        pty bridge is added, so the machine pumps it at slice boundaries."""
        self._aliases = dict(aliases)
        self._dropped_base: dict[str, int] = {}
        self._connect = connect
        self._wait = wait
        self._post = post
        self._bridged = bridged
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
            link = self.link(name)
            stream = UartStream(
                name,
                link,
                self._wait,
                self.bridges,
                self._post,
                lambda: link.dropped() - self._dropped_base.get(name, 0),
            )
            self._streams[name] = stream
        return stream

    def pump(self) -> None:
        """Move pty bytes at a slice boundary. The machine calls it on its thread."""
        for bridge in list(self.bridges.values()):
            bridge.pump()

    def bridge(self, key: str, make: Callable[[Callable[[], None]], PtyBridge]) -> PtyBridge:
        """The bridge keyed `key`, made by `make(wake)` on first use."""
        found = self.bridges.get(key)
        if found is None:
            found = self.bridges[key] = make(self.wake)
            self._bridged()
        return found

    def wake(self) -> None:
        """Make a running machine take a slice, so host input reaches it."""
        self._post(lambda: None)

    def discard(self) -> None:
        """Drop transmitted frames not read yet and bytes waiting for ptys, at a restore."""
        for name, link in self._links.items():
            link.drain(link.available())
            self._dropped_base[name] = link.dropped()
        for bridge in self.bridges.values():
            bridge.discard()


class Board:
    """What is wired to the MCU: host ptys bridged to its UARTs and USB CDC functions, and the
    Python devices on its buses, `m.board`.

    Every built-in UART is connected to Python from power-on: its `port` holds what the
    firmware sent since power-on or the last `Machine.restore`, unless a pty bridge takes it.
    """

    __slots__ = ("_devices", "_ports", "_usb")

    def __init__(
        self,
        ports: UartPorts,
        devices: Mapping[str, tuple[BusDevice, ...]],
        usb: UsbHosts | None = None,
    ) -> None:
        """Bridge the UARTs of `ports` and the CDC functions of the hosts of `usb`, list
        `devices`. `Machine.board` builds it."""
        self._ports = ports
        self._devices = dict(devices)
        self._usb = usb

    def devices(
        self, peripheral: str
    ) -> tuple[I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget, ...]:
        """The devices on bus `peripheral`, in attachment order, empty for none."""
        return cast(
            "tuple[I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget, ...]",
            self._devices.get(peripheral, ()),
        )

    def pty(self, name: str, *, function: int = 0, deadlock_warning: float = 2.0) -> str:
        """Bridge UART `name`, or a CDC ACM function of the virtual USB host of controller
        `name`, to a new host pty and return its path, the same one on later calls.

        The pty owns what the firmware sends, for the life of the machine: reads of the UART's
        `port`, or of the CDC function's `host.cdc()` stream, raise
        `RuntimeError("<key> is bridged to <path>")`, the key as in `ptys`.
        `Periph(connect="pty")` in a `Project` calls this at build for every machine of the
        project. Call it from the test that wants the pty instead, to keep stream reads in the
        others. A CDC function's pty is keyed `name` in `ptys` for function 0, `name.<function>`
        for the others.

        Args:
            name: UART instance, or USB device controller with a virtual host.
            function: The n-th CDC ACM function of the device, in interface order. 0 for a
                UART.
            deadlock_warning: Wall seconds host input may wait with no slice run before a
                warning names the pty.

        Raises:
            ValueError: `function` is negative, above 0 for a UART, or beyond the CDC ACM
                functions of a configuration already read.
            ModelError: The peripheral's model is neither a UART nor a USB device controller
                with a virtual host.
            ConfigError: The description has no such peripheral.
        """
        if self._usb is not None and self._usb.serves(name):
            link = self._usb.cdc_link(name, function)
            key = cdc_key(self._usb.host(name).name, function)
            return self._ports.bridge(
                key, lambda wake: PtyBridge(key, link, deadlock_warning, wake)
            ).path
        if function != 0:
            raise ValueError(f"{name}: a UART has no function {function}, only 0")
        name = self._ports.serving(name)
        uart = UartPtyLink(self._ports.link(name))
        return self._ports.bridge(
            name, lambda wake: PtyBridge(name, uart, deadlock_warning, wake)
        ).path

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


def _close(watch: list[asyncio.AbstractEventLoop | None], lock: threading.Lock, *fds: int) -> None:
    loop = watch[0]
    if loop is not None and loop.is_running() and not _running_here(loop):
        with contextlib.suppress(RuntimeError):
            loop.call_soon_threadsafe(_close_now, watch, lock, *fds)
            return
    _close_now(watch, lock, *fds)


def _running_here(loop: asyncio.AbstractEventLoop) -> bool:
    try:
        return asyncio.get_running_loop() is loop
    except RuntimeError:
        return False


def _close_now(
    watch: list[asyncio.AbstractEventLoop | None], lock: threading.Lock, *fds: int
) -> None:
    _unwatch(watch, fds[0])
    with lock:
        for fd in fds:
            with contextlib.suppress(OSError):
                os.close(fd)
