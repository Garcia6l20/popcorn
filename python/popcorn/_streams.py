"""UART byte streams in virtual time, and host ptys bridged to UARTs and USB CDC ACM
functions."""

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
from typing import TYPE_CHECKING, Protocol, cast

from popcorn import _core
from popcorn._devices import BusDevice, I2CTarget, SpiTarget
from popcorn.devices import At25sf, Eeprom24xx, TableTarget
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._usb import UsbHosts

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
    until read, about 16 bytes each, up to 1 Mi frames, about 16 MiB. A full queue drops its
    oldest frame and counts it in `dropped`.

    `Machine.restore` empties the queue of transmitted frames not read yet, since they came
    from a run the restore undoes. Bytes written and not yet delivered are part of the UART
    model's state and return to what the snapshot holds.
    """

    __slots__ = ("__weakref__", "_bridges", "_dropped", "_link", "_stimulus", "_wait", "name")

    def __init__(
        self,
        name: str,
        link: _core.UartLink,
        wait: Waiter,
        bridges: dict[str, PtyBridge],
        stimulus: Callable[[], None],
        dropped: Callable[[], int],
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.port` instead."""
        self.name = name
        self._dropped = dropped
        self._link = link
        self._wait = wait
        self._bridges = bridges
        self._stimulus = stimulus

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

    def __init__(self, name: str, link: PtyLink, deadlock_warning: float) -> None:
        """Open a pty for `name` over `link`. Use `m.board.pty(name)` instead."""
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

    __slots__ = (
        "_aliases",
        "_connect",
        "_dropped_base",
        "_links",
        "_stimulus",
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
        stimulus: Callable[[], None],
    ) -> None:
        """Connect every UART named in `uarts` through `connect` at once, so each one queues
        what the firmware transmits from power-on. `aliases` maps an alias, such as a secure
        address window, to the peripheral serving it, which shares its link. Reads run the
        machine through `wait`, writes are checked by `stimulus`."""
        self._aliases = dict(aliases)
        self._dropped_base: dict[str, int] = {}
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
            link = self.link(name)
            stream = UartStream(
                name,
                link,
                self._wait,
                self.bridges,
                self._stimulus,
                lambda: link.dropped() - self._dropped_base.get(name, 0),
            )
            self._streams[name] = stream
        return stream

    def pump(self) -> None:
        """Move pty bytes at a slice boundary. The machine calls it."""
        for bridge in self.bridges.values():
            bridge.pump()

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

        A bridged UART is read by the pty only: its `port` refuses reads, as the `cdc()` stream
        of a bridged CDC function does. A CDC function's pty is keyed `name` in `ptys` for
        function 0, `name.<function>` for the others.

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
        bridges = self._ports.bridges
        if self._usb is not None and self._usb.serves(name):
            link = self._usb.cdc_link(name, function)
            key = cdc_key(self._usb.host(name).name, function)
            bridge = bridges.get(key)
            if bridge is None:
                bridge = PtyBridge(key, link, deadlock_warning)
                bridges[key] = bridge
            return bridge.path
        if function != 0:
            raise ValueError(f"{name}: a UART has no function {function}, only 0")
        name = self._ports.serving(name)
        bridge = bridges.get(name)
        if bridge is None:
            bridge = PtyBridge(name, UartPtyLink(self._ports.link(name)), deadlock_warning)
            bridges[name] = bridge
        return bridge.path

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


def _close(watch: list[asyncio.AbstractEventLoop | None], *fds: int) -> None:
    _unwatch(watch, fds[0])
    for fd in fds:
        with contextlib.suppress(OSError):
            os.close(fd)
