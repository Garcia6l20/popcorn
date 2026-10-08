"""UART byte streams in virtual time, and host ptys bridged to UARTs and USB CDC ACM
functions."""

from __future__ import annotations

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

type Waiter = Callable[[_core.Predicate, VirtualDuration | None], Awaitable[VirtualTime]]
"""Runs the machine until a native predicate holds or a virtual duration elapses, and returns
the virtual time it stopped waiting at. The predicate runs on the machine's thread with no GIL."""


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
        return written(self._post(link.command_send(data), at))

    async def read(self, n: int, *, for_: VirtualDuration | None = None) -> bytes:
        """Run the machine until `n` bytes arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine. The
        run stops as `Machine.wait_for` does: after the last frame's own time, at the first event
        boundary or slice end, or past `for_` by the bound `Machine.run` gives for its `for_`.

        Returns:
            Up to `n` bytes, fewer when `for_` elapsed or the core went idle first.

        Raises:
            RuntimeError: The UART is bridged to a pty, which takes what the firmware sends.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
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
            await self._wait(self._link.predicate_available(n), for_)
        return [Frame(f.data, VirtualTime(f.at_ps)) for f in self._link.drain(n)]

    def __repr__(self) -> str:
        return f"UartStream({self.name})"


type PtyOpener = Callable[[str, _core.UartLink | _core.CdcLink], _core.PtyBridge]
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
    )

    def __init__(
        self,
        connect: Callable[[str], _core.UartLink],
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

    def bridge(self, key: str, link: _core.UartLink | _core.CdcLink) -> PtyBridge:
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

    def pty(self, name: str, *, function: int = 0) -> str:
        """Bridge UART `name`, or a CDC ACM function of the virtual USB host of controller
        `name`, to a new host pty and return its path, the same one on later calls.

        The pty owns what the firmware sends, for the life of the machine: reads of the UART's
        `port`, or of the CDC function's `host.cdc()` stream, raise
        `RuntimeError("<key> is bridged to <path>")`, the key as in `ptys`.
        `Periph(connect="pty")` in a `Project` calls this at build for every machine of the
        project. Call it from the test that wants the pty instead, to keep stream reads in the
        others. A CDC function's pty is keyed `name` in `ptys` for function 0, `name.<function>`
        for the others.

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
