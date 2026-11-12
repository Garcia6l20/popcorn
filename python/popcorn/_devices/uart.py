from __future__ import annotations

import typing
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import Literal, cast, override

from popcorn import _core
from popcorn._devices.base import parked, saved, serving
from popcorn._devices.naming import refuse_reserved
from popcorn._devices.pins import DeviceBoard
from popcorn._devices.watches import Unwatch
from popcorn._enums import Parity, ResetKind, UartError
from popcorn._errors import ConfigError, MachineRunning, ModelError, SnapshotRefused
from popcorn._limits import MAX_PS
from popcorn._tap import TapFrame
from popcorn._virtual_loop import VirtualLoop
from popcorn.units import VirtualTime

if typing.TYPE_CHECKING:
    pass

_TAP_KEEP = 1 << 20


_MAX_BAUD = 100_000_000


@dataclass(frozen=True)
class LineFormat:
    """The line format of a `UartDevice`: 8 data bits, a parity bit if any, then the stop bits.

    Attributes:
        baud: Bits per second, 1 to 100 000 000.
        parity: The parity bit after the data bits.
        stop_bits: Stop bits, 1 or 2.
    """

    baud: int
    parity: Parity = Parity.NONE
    stop_bits: Literal[1, 2] = 1

    def __post_init__(self) -> None:
        if not isinstance(cast(object, self.baud), int) or isinstance(
            cast(object, self.baud), bool
        ):
            raise TypeError(f"baud is an int, not {self.baud!r}")
        if not 1 <= self.baud <= _MAX_BAUD:
            raise ValueError(f"baud is 1 to {_MAX_BAUD}, not {self.baud}")
        if not isinstance(cast(object, self.parity), Parity):
            raise TypeError(f"parity is a Parity, not {self.parity!r}")
        if type(cast(object, self.stop_bits)) is not int or self.stop_bits not in (1, 2):
            raise ValueError(f"stop_bits is 1 or 2, not {self.stop_bits!r}")


class UartDevice(_core.UartDeviceBase):
    """The far end of a USART, UART or LPUART, written in Python: it hears what the firmware
    transmits on the machine's thread at each frame's virtual time, and sends at the baud the
    firmware configured.

    Attach it with `Machine(devices={"USART1": [device]})` or a machine description's
    `Periph(devices=[Device(cls)])`, alone on its UART. The UART then has no `UartStream`, no pty
    and no link of a `System`, each refused with `ConfigError`.

    Thread: `on_attach`, `on_frame`, `on_break`, `on_rts` and `on_error` run on the machine's
    thread with the GIL, one GIL entry per call, each inside a step of the machine's `VirtualLoop`,
    the `loop` here. `get_running_loop()` returns it there, and a task made there takes its first
    step at the same stamp. All of them share the machine's one context. `reset` runs where the
    reset was called, also in the loop, and what it schedules runs at the machine's next dispatch.
    `send`, `send_break` and `drive_cts` work from the hooks and from the loop's callbacks and
    tasks. They raise inside `reset`.

    Time: `at` and `now` count from the last reset of any kind, as `Machine.time`. Inside a step
    `now` is its logical stamp, the `at` of the hook or the due time of the timer: the event runs at
    the first block boundary at or after it, so `Machine.time` can read up to one block later, and
    a reply placed from `now` keeps its exact distance from the frame. A stamp a device or a timer
    set is exact at any block shape. A register access stamps at the end of its block: `on_error`, a
    rising `on_rts`, and a frame the firmware started, whose end counts from its store.

    Order: hooks and timers run as machine events in instruction count order. At one count hooks
    run in stamp order, at one stamp lane, then post order decides, the frame hooks ahead of
    `on_rts` and `on_error`. The timers run behind every other event of their count, and a hook's
    `at` is held at the last stamp, so `at == now` in every hook and `time()` never goes back. `at`
    moves by under one instruction period, only when a timer shares a count with a hook of a later
    stamp, the same way on every run. That timer runs in the hook's step, at that stamp, under one
    instruction period past its own.

    Frames: a frame reaches `on_frame` with its data bits, parity stripped, at its stop-bit end,
    the time `Frame.at` gives on a `UartStream`. With no `line` the device is in the firmware's
    format: the transmitter always sends a well-formed frame at its own settings, so the one line
    error it hears is `UartError.ABORTED`.

    Line: `line` gives the device a format of its own, `LineFormat(baud, parity, stop_bits)` with 8
    data bits. `send` shapes each frame in it, taking the format at the call. The firmware's UART
    reads the frame by sampling its waveform at the UART's bit times, three samples per bit at 7,
    8 and 9 sixteenths with a majority vote, the model's sampler, so a frame in the same format
    reads back as sent and a frame in another baud, parity or stop length reads as the sampler
    gives, as for a frame from a linked machine: its bytes and flags are the model's sampler's
    and inferred, no manual states the outcome of a baud mismatch.
    A `line` with the UART's word shape and frame time, to a picosecond, is no mismatch: it reads
    and arrives as no `line` does, the same bytes at the same stamps. The device hears
    the firmware's frames through the same sampler at its own bit times, the format read when the
    frame's stop bit ends. That is the model's sampler applied to the other end, and inferred
    too: a real chip may sample differently. A frame read without a framing or parity error
    reaches `on_frame` with the sampled byte, also when only noise was sampled. A framing or
    parity error reaches `on_error` only, as `UartError.FRAMING`, or `PARITY` when the stop bit
    read 1. Both carry the stamp of the firmware's stop-bit end, whatever the device's
    sampling window. A sampling window that reaches past the end of the frame reads the next
    start bit of a frame following it as low. A break reaches `on_break` in every
    format. A tap shows each frame as it was on the wire, the device's frames at its format's
    frame time.

    Restore: a device's Python attributes are never rewound, so a stack keeps its state in what
    `on_attach` builds, or in the bytes of `snapshot` that `restore` reads. State set in `__init__`
    survives a restore, so `on_attach` must set every attribute the stack changes, or a rerun starts
    from the last run's values and can answer twice. A restore empties the
    loop, see `VirtualLoop`, and then calls `restore`, whose default runs `on_attach` again in the
    loop's event at the restored clock, behind every event pending at it. A rerun starts the stack
    from its first state and its timers from the restored clock, so the stack's stamps equal the
    first run's only for a snapshot at the attach stamp, and a fixture boot gets the stamps of a
    stack started at `boot.until`.

    The default `snapshot` raises `SnapshotRefused` once traffic crossed since the last attach, what
    a rerun cannot rebuild: a `send`, a `send_break`, a `drive_cts`, an `on_frame`, an `on_break`,
    an `on_error`, an `on_rts` delivered to a class that overrides it, and past the attach stamp a
    `drive_pin`, an edge delivered to a `watch_pin` watch or a rate delivered to a `watch_pin_clock`
    watch made from the loop. During the rerun `send`, `send_break` and `drive_cts` raise
    `RuntimeError`, from `on_attach` and from what runs at its stamp. A timer it arms for later is
    the device's own, and acts when it fires. The rerun ends the `watch_pin` and `watch_pin_clock`
    watches the device made from the loop first, as `on_attach` makes them again. A device whose
    stack talks at boot, or whose `on_attach` builds no whole stack, overrides `snapshot` and
    `restore`. `System.restore` takes a snapshot of each member ahead of any change, so a device
    that refuses raises there too, naming the device, with nothing changed: a member whose default
    stack heard traffic, an edge included, cannot restore even a quiet snapshot, where a lone
    machine can.

    Tap: `tapping = True` records every frame of the port in both directions as a `TapFrame`, off
    by default. The frames the firmware sent are `towards` `"device"`, with the start-bit and
    stop-bit times of the UART's transmitter, a break ending at its last low bit, a frame the
    firmware aborted `lost` with the time of the abort, an aborted break as lost data 0. The
    frames the device sent are `towards` the UART's name, with the start-bit and stop-bit times
    of the frame on the wire, noted when its stop-bit time passes, a frame the receiver drops
    included. Each start is kept from when the frame began, so a baud change or a restore in the
    middle of a frame leaves it as the run that went on. The times are the native ones and
    `arrival` equals `end`: a hook's `at` is held at the last stamp and can differ. `index`
    counts the frames tapped in a direction since tapping began or since the last restore. A tap
    keeps 1 Mi frames per direction not taken, about 440 bytes each, the oldest dropped and
    counted in `tap_dropped`. `tap_frames` takes them. Frames tapped before a machine reset stay,
    with the times of the epoch they were seen in, and `index` goes on. A restore drops them, in
    flight ones included, and restarts `index` and `tap_dropped` at 0, whatever the snapshot held
    for this device. Tapping keeps its setting through both and costs one GIL entry per frame
    the device is sent. Switching it from a thread while the machine runs takes effect at each
    direction's next frame.

    Reset: a device on the board is not reset with the MCU. `reset` is told every machine reset and
    keeps the device's state by default. The frames `send` queued and not delivered are dropped by
    any machine reset. The loop's timers run on through a reset, and `rts` reads True with no edge,
    so a stack holding RTS False or timing the firmware resets that in `reset`.

    Errors: an exception from a hook stops the run as a `ModelError` naming the device, once the
    step has finished. `VirtualLoop.set_exception_handler` takes it over.

    Lifetime: a device serves one machine at a time and must not keep a reference to it. It may
    keep its `board`, which holds no machine.

    Names: a name starting with `_` that this class defines is reserved. A subclass defining one
    raises TypeError at class definition, and an instance attribute with one raises TypeError when
    the machine takes the device. Other `_` names are free for the subclass, and an attribute a
    subclass sets never replaces the class's own state, which is name-mangled.
    """

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        refuse_reserved(cls, UartDevice)

    def __init__(self, *, line: LineFormat | None = None) -> None:
        """Make a device serving no machine yet.

        Args:
            line: The device's own format, None for the firmware's UART format.

        Raises:
            TypeError: `line` is neither a `LineFormat` nor None.
        """
        super().__init__(type(self).on_rts is not UartDevice.on_rts)
        self.__line: LineFormat | None = None
        self.line = line
        self.__link: _core.UartDeviceLink | None = None
        self.__loop: VirtualLoop | None = None
        self.__refusing = ""
        self.__peripheral = ""
        self.__traffic = False
        self.__held: tuple[bool, bytes] | None = None
        self.__reattach = False
        self.__rerun_stamp: int | None = None
        self.__attach_stamp: int | None = None
        self.__watches: list[Unwatch] = []
        self.__tapping = False
        self.__tapped = (deque[TapFrame](maxlen=_TAP_KEEP), deque[TapFrame](maxlen=_TAP_KEEP))
        self.__tap_index = [0, 0]
        self.__tap_lost = 0

    @property
    def line(self) -> LineFormat | None:
        """The device's own format, None for the firmware's UART format. Set it with the machine
        parked, or from a hook or a loop callback. A running machine refuses a set from the test
        thread with `MachineRunning`, since the format decides which frame reads in which format:
        `loop.post(setattr, device, "line", line, at=)` sets it at a stamp. A frame `send` queued
        keeps the format it was queued in, and the next frame the firmware sends is read in the
        format set when its stop bit ends. The attribute is Python state and survives a restore.

        Raises:
            TypeError: `line` is neither a `LineFormat` nor None.
            MachineRunning: The machine is running and the caller is neither a hook nor a loop
                callback.
        """
        return self.__line

    @line.setter
    def line(self, line: LineFormat | None) -> None:
        if line is not None and not isinstance(cast(object, line), LineFormat):
            raise TypeError(f"line takes a LineFormat or None, not {line!r}")
        args = self._line_args(line)

        def store() -> None:
            self.set_line(*args)
            self.__line = line

        try:
            parked(self, store)
        except MachineRunning as error:
            raise MachineRunning(
                f"{self!r}.line is set on a machine that is running: use"
                ' loop.post(setattr, device, "line", line, at=) to set it at a stamp'
            ) from error

    @staticmethod
    def _line_args(line: LineFormat | None) -> tuple[int, int, int]:
        if line is None:
            return 0, 0, 2
        return line.baud, line.parity.value, 2 * line.stop_bits

    @property
    def tapping(self) -> bool:
        """Whether the port records its frames, see the class doc. Off by default, settable from
        any thread."""
        return self.__tapping

    @tapping.setter
    def tapping(self, on: bool) -> None:
        if not isinstance(cast(object, on), bool):
            raise TypeError(f"tapping takes a bool, not {on!r}")
        self.set_tapping(on)
        self.__tapping = on

    @property
    def tap_dropped(self) -> int:
        """Frames the tap dropped because it held its limit, since the last restore."""
        return self.__tap_lost

    def tap_frames(self, towards: str | None = None) -> list[TapFrame]:
        """Take the frames the tap holds, oldest first, and forget them.

        Args:
            towards: `"device"` for the frames the firmware sent, the UART's name for the frames
                the device sent, None for both, ordered by `end` and the firmware's first at a tie.

        Raises:
            TypeError: `towards` is no str or None.
            ValueError: `towards` names neither end.
            ConfigError: The device serves no live machine and `towards` names the UART.
        """
        if towards is not None and not isinstance(cast(object, towards), str):
            raise TypeError(f"tap_frames() takes towards as a str or None, not {towards!r}")
        if towards is None:
            taken = [*self._take_tapped(0), *self._take_tapped(1)]
            taken.sort(key=lambda frame: (frame.end.ps, frame.towards != "device"))
            return taken
        if towards == "device":
            return self._take_tapped(0)
        if self.__peripheral and towards == self.__peripheral:
            return self._take_tapped(1)
        raise ValueError(f"towards {towards!r} is neither 'device' nor {self.__peripheral!r}")

    def _take_tapped(self, direction: int) -> list[TapFrame]:
        held = self.__tapped[direction]
        taken: list[TapFrame] = []
        while held:
            taken.append(held.popleft())
        return taken

    def _record(
        self, direction: int, data: int, start: int, end: int, *, brk: bool, lost: bool
    ) -> None:
        if not self.__tapping:
            return
        held = self.__tapped[direction]
        if len(held) == _TAP_KEEP:
            self.__tap_lost += 1
        index = self.__tap_index[direction]
        self.__tap_index[direction] = index + 1
        held.append(
            TapFrame(
                data=data,
                index=index,
                start=VirtualTime(start),
                end=VirtualTime(end),
                arrival=VirtualTime(end),
                towards="device" if direction == 0 else self.__peripheral,
                injected=False,
                brk=brk,
                lost=lost,
                dropped=False,
                cut=False,
            )
        )

    @property
    def board(self) -> DeviceBoard:
        """What the device may drive on the board of the machine it serves."""
        return DeviceBoard(self)

    @property
    def loop(self) -> VirtualLoop:
        """The virtual-time loop of the machine the device serves, `Machine.loop`.

        Raises:
            ConfigError: The device serves no live machine.
        """
        self._link_of()
        loop = self.__loop
        assert loop is not None
        return loop

    @property
    def now(self) -> VirtualTime:
        """The logical stamp of the running loop step, the `at` of a hook or the due time of a
        timer, else `Machine.time`.

        Raises:
            ConfigError: The device serves no live machine.
            MachineRunning: Read off the machine's thread while it runs.
        """
        loop = self.__loop
        if loop is not None and loop._stepping_here():  # pyright: ignore[reportPrivateUsage]
            return VirtualTime(loop._stamp_since_reset())  # pyright: ignore[reportPrivateUsage]
        machine = serving(self)
        if machine is None:
            raise self._serves_none()
        return cast(VirtualTime, getattr(machine, "time"))  # noqa: B009

    def on_attach(self) -> None:
        """The machine runs for the first time since the device was attached, at the first event
        boundary. The default `restore` calls it again after a restore of a snapshot taken after
        that first call, in the same machine or in a new one."""

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        """The firmware transmitted a frame holding `byte`, 0 to 0x1FF, its stop bit ending at
        `at`. With a `line`, the byte is what the device's sampler read, 0 to 0xFF."""

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        """A line error: `UartError.ABORTED` at the time the firmware cut the frame, the end of the
        block holding the write, or `Machine.time` for a parked write. With a `line`, also
        `UartError.FRAMING` or `PARITY` for a frame read with that error, at its stop-bit end. It
        is a machine event at that stamp, so it runs on the machine's thread at the next dispatch, a
        timer due earlier firing ahead of it, and an exception from it stops that run rather than
        the write. A machine reset before that dispatch drops it, and `reset` tells the device."""

    def reset(self, kind: ResetKind) -> None:
        """The machine was reset. Does nothing by default. The machine's first reset comes before
        `on_attach`, so what `on_attach` builds is absent then."""

    def snapshot(self) -> bytes:
        """The device state as bytes for `restore`. By default none.

        Raises:
            SnapshotRefused: By default, when traffic crossed since the last attach, see the class
                doc. Override this and `restore` for such a device.
        """
        self._check_quiet()
        return b""

    def restore(self, data: bytes) -> None:
        """A snapshot was restored, `data` what `snapshot` returned for it. It runs in the loop on
        the thread that called `restore`, at the restored clock, once the machine has finished its
        restore, the loop emptied, in the context the hooks run in. So a `call_later` here counts
        from the restored clock, and what it schedules runs at the machine's next dispatch. `send`,
        `send_break` and `drive_cts` raise here. By default it runs `on_attach` again in the loop's
        event at the restored clock, behind every event pending at it, when the snapshot was taken
        after the first one, and an override runs it only by calling this. That rerun starts the
        stack's timers from the restored clock, so its stamps equal the first run's only for a
        snapshot at the attach stamp. It ends the watches the device made from the loop, those made
        here included, so a device calling this watches in `on_attach`. An edge pending at the
        restored clock reaches the old stack's watches ahead of the rerun and what it arms stays, so
        a stack that arms from the edge and from the level in `on_attach` checks it armed once. An
        exception raises from the machine's `restore` as a `ModelError`, once that has finished."""
        loop = self.__loop
        if (
            self.__reattach
            and type(self).on_attach is not UartDevice.on_attach
            and loop is not None
            and not loop.is_closed()
        ):
            loop._call_at_fire(self._attach_again)  # pyright: ignore[reportPrivateUsage]

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        """The firmware's RTS changed at `at`, `ready` when its receiver can take a frame. RTS
        changes with the receiver's not-empty flag, which the model sets at the stop-bit end,
        inside the stop bit on silicon. Called once per edge, in stamp order with the other hooks,
        the frame hooks first at a tie.
        A class that overrides this hears the edges, and any other costs no GIL entry per edge. The
        choice is the class's, fixed at construction, so a stack built in `on_attach` that registers
        its handler later needs the override, forwarding to it.
        A machine reset sets the level True with no edge, so a device tracking it also uses
        `reset`. A delivery to a class that overrides this counts as traffic, so the default
        `snapshot` refuses, since a stack started again would lose what the edge told it. The base
        hook counts nothing, and a rerun reads `rts`."""

    def on_break(self, at: VirtualTime) -> None:
        """The firmware sent a break, its last low bit ending at `at`."""

    @property
    def rts(self) -> bool:
        """Whether the firmware's RTS says ready: False only with CR3.RTSE 1, CR3.DEM 0 and the
        receiver full, True on a UART without flow control.

        Raises:
            ConfigError: The device serves no live machine.
            MachineRunning: Read off the machine's thread while it runs.
        """
        return self._link_of().rts_ready()

    def send(self, data: bytes, *, at: VirtualTime | None = None, obey_rts: bool = True) -> None:
        """Queue frames of 8 data bits towards the firmware's receiver, the first start bit at
        `at`, or at `now` when None, the others back to back. `at` behind frames still queued
        starts when they end. With no `line`, each frame lasts the frame time of the UART's
        settings at its start, so a baud change between frames applies from the next one. With a
        `line`, each frame lasts that format's frame time and keeps it, whatever happens to `line`
        or to the UART's settings before it is read, and the UART reads it by sampling. It reaches
        the receiver at the later of its stop-bit end and the UART's last stop sample.

        RTS: with `obey_rts`, a frame waits before its start bit while `rts` is False, and starts
        no earlier than the edge that sets it True, whatever its `at`. Without it the frame starts
        at its time and overruns a full receiver, setting ORE as on silicon. Frames start in the
        order they were queued, so it still waits behind a frame that RTS holds.
        A frame with a `line` starts at the end of the one before it on the line when RTS reads
        ready before that one is delivered, and otherwise waits for RTS. A device slower than the
        UART is delivered at its wire end, so a full receiver holds the next frame. A device
        faster than the UART is delivered after its wire end, RTS has not fallen when the next
        frame starts, and a receiver that is full then overruns.

        Raises:
            TypeError: `data` is no bytes, `at` no `VirtualTime`, or `obey_rts` no bool.
            ValueError: `at` is before `now`, or leaves the 64-bit picosecond range.
            RuntimeError: Called inside `reset` or `restore`, on any path they take, while a restore
                empties the loop, while `on_attach` runs again, or off the machine's thread outside
                the device's other hooks.
            ConfigError: The device serves no live machine.
        """
        if not isinstance(cast(object, data), bytes | bytearray | memoryview):
            raise TypeError(f"send() takes bytes, not {type(data).__name__}")
        if not isinstance(cast(object, obey_rts), bool):
            raise TypeError(f"send() takes obey_rts as a bool, not {obey_rts!r}")
        link, start = self._acting("send", at)
        link.send(bytes(data), start, not obey_rts, *self._line_args(self.__line))
        self.__traffic = True

    def send_break(self, *, at: VirtualTime | None = None) -> None:
        """Queue one break towards the firmware's receiver after the frames queued, at `at` or
        `now` when None, placed as `send` places a frame and held by RTS the same way. It reads as a
        0 byte with the framing error flag, and holds the line one frame time plus 2 bit times.

        Raises:
            TypeError: `at` is no `VirtualTime`.
            ValueError: As `send`.
            RuntimeError: As `send`.
            ConfigError: As `send`.
        """
        link, start = self._acting("send_break", at)
        link.send_break(start)
        self.__traffic = True

    def drive_cts(self, ready: bool, *, at: VirtualTime | None = None) -> None:
        """Drive the firmware's nCTS input to `ready` at `at`, or at `now` when None. With CR3.CTSE,
        a frame in flight completes and the next waits for `ready`. The level and the changes
        pending are board state, kept across every machine reset at their instant.

        Raises:
            TypeError: `ready` is no bool, or `at` no `VirtualTime`.
            ValueError: As `send`.
            RuntimeError: As `send`.
            ConfigError: As `send`, the UART has no flow control, or at most 65536 changes pend.
        """
        if not isinstance(cast(object, ready), bool):
            raise TypeError(f"drive_cts() takes ready as a bool, not {ready!r}")
        link, start = self._acting("drive_cts", at)
        if not link.has_flow_control():
            raise ConfigError(
                f"{self!r}: this UART has no CTS",
                key=f"devices.{self.__peripheral}",
                expected="a UART with hardware flow control",
            )
        link.set_cts(ready, start)
        self.__traffic = True

    def _link_of(self) -> _core.UartDeviceLink:
        link = self.__link
        if link is None or serving(self) is None:
            raise self._serves_none()
        return link

    def _acting(self, what: str, at: VirtualTime | None) -> tuple[_core.UartDeviceLink, int]:
        """The link and the time of an action `what` at `at`, refused where it cannot act."""
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"{what}() takes at as a VirtualTime or None, not {at!r}")
        link = self._link_of()
        if self.__refusing:
            raise RuntimeError(f"{self!r}: {what}() is refused inside {self.__refusing}()")
        loop = self.__loop
        stepping = loop is not None and loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
        if stepping:
            assert loop is not None
            if loop._is_emptying():  # pyright: ignore[reportPrivateUsage]
                raise RuntimeError(
                    f"{self!r}: {what}() is refused while a restore empties the loop"
                )
            if self.__rerun_stamp == loop._now_ps:  # pyright: ignore[reportPrivateUsage]
                raise RuntimeError(f"{self!r}: {what}() is refused while on_attach runs again")
        if not stepping and not _core.on_host_thread():
            raise RuntimeError(f"{self!r}: {what}() runs on the machine's thread")
        now = self.now.ps
        start = now if at is None else at.ps
        if start < now:
            raise ValueError(f"{self!r}: {what}() at {at} lies before now, {VirtualTime(now)}")
        if start > MAX_PS:
            raise ValueError(f"a {what} at {at} leaves the 64-bit picosecond range")
        return link, start

    def _bind(self, link: _core.UartDeviceLink, peripheral: str, loop: VirtualLoop) -> None:
        self.__link = link
        self.__peripheral = peripheral
        self.__loop = loop

    def _note_traffic(self) -> None:
        self.__traffic = True

    def _serves_none(self) -> ConfigError:
        return ConfigError(
            f"{self!r} serves no live machine",
            key="devices",
            expected="a device serving a live machine",
        )

    def _attached(self, at: int) -> None:
        loop = self.__loop
        assert loop is not None
        loop._run_hook(at, self._first_attach)  # pyright: ignore[reportPrivateUsage]

    def _first_attach(self) -> None:
        loop = self.__loop
        assert loop is not None
        self.__attach_stamp = loop._now_ps  # pyright: ignore[reportPrivateUsage]
        self._called(self.on_attach)

    def _frame(self, data: int, at: int, start: int, wire: int) -> None:
        self.__traffic = True
        self._record(0, wire, start, at, brk=False, lost=False)
        self._stamped(at, self.on_frame, data)

    def _error(self, kind: int, at: int, start: int, wire: int) -> None:
        self.__traffic = True
        self._record(0, wire, start, at, brk=False, lost=False)
        self._stamped(at, self.on_error, UartError(kind))

    def _break(self, at: int, start: int) -> None:
        self.__traffic = True
        self._record(0, 0, start, at, brk=True, lost=False)
        self._stamped(at, self.on_break)

    def _received(self, data: int, start: int, end: int, brk: bool) -> None:
        self._record(1, data, start, end, brk=brk, lost=False)

    def _rts(self, ready: bool, at: int) -> None:
        self.__traffic = True
        self._stamped(at, self.on_rts, ready)

    def _aborted(self, at: int, start: int, data: int) -> None:
        self.__traffic = True
        self._record(0, data, start, at, brk=False, lost=True)
        self._stamped(at, self.on_error, UartError.ABORTED)

    def _stamped(self, at: int, hook: Callable[..., None], *args: object) -> None:
        loop = self.__loop
        assert loop is not None

        def held_at_the_stamp() -> None:
            stamp = VirtualTime(loop._stamp_since_reset())  # pyright: ignore[reportPrivateUsage]
            self._called(hook, *args, stamp)

        loop._run_hook(at, held_at_the_stamp)  # pyright: ignore[reportPrivateUsage]

    def _reset(self, kind: _core.ResetKind) -> None:
        refusing = self.__refusing
        self.__refusing = "reset"
        try:
            loop = self.__loop
            if loop is None:
                self._called(self.reset, ResetKind(kind.value))
            else:
                loop._run_alone(  # pyright: ignore[reportPrivateUsage]
                    0, self._called, self.reset, ResetKind(kind.value)
                )
        finally:
            self.__refusing = refusing

    def _save(self) -> bytes:
        data = saved(self, typing.cast(object, self.snapshot()))
        return (b"\x01" if self.__traffic else b"\x00") + data

    def _load(self, attached: bool, data: bytes) -> None:
        self.__held = (attached, data)

    def _check_quiet(self) -> None:
        if self.__traffic:
            raise SnapshotRefused(
                f"{self!r}: traffic crossed the wire since its last attach, so running on_attach"
                " again cannot rebuild its stack: override snapshot() and restore()",
                device=repr(self),
            )

    def _note_pin(self, later: bool = False) -> None:
        """A pin crossed for this device: traffic once its stack has attached, past that stamp, or
        at it for a drive due `later`. A drive from `restore` repeats at every restore."""
        loop = self.__loop
        if self.__attach_stamp is None or loop is None:
            return
        if self.__refusing == "restore" and not later:
            return
        stepping = loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
        now = loop._now_ps  # pyright: ignore[reportPrivateUsage]
        if stepping and now == self.__attach_stamp and not later:
            return
        self.__traffic = True

    def _end_watches(self) -> None:
        for watch in self.__watches:
            watch.remove()
        self.__watches.clear()

    def _rebuild(self) -> None:
        """Call `restore` with the bytes the machine's restore loaded."""
        for taken in self.__tapped:
            taken.clear()
        self.__tap_index = [0, 0]
        self.__tap_lost = 0
        held = self.__held
        if held is None:
            return
        self.__held = None
        attached, data = held
        loop = self.__loop
        assert loop is not None
        self.__traffic = data[0] != 0
        self.__reattach = attached
        self.__rerun_stamp = None
        self.__attach_stamp = -1 if attached else None
        if not attached:
            self._end_watches()
        self.__refusing = "restore"
        try:
            loop._run_alone(  # pyright: ignore[reportPrivateUsage]
                0, self._called, self.restore, data[1:]
            )
        finally:
            self.__refusing = ""

    def _note_watch(self, watch: Unwatch) -> None:
        self.__watches = [standing for standing in self.__watches if standing.active]
        self.__watches.append(watch)

    def _attach_again(self) -> None:
        loop = self.__loop
        assert loop is not None
        self.__rerun_stamp = loop._now_ps  # pyright: ignore[reportPrivateUsage]
        self.__attach_stamp = self.__rerun_stamp
        self._end_watches()
        self._called(self.on_attach)

    def _called[**P](self, hook: Callable[P, None], *args: P.args, **kwargs: P.kwargs) -> None:
        try:
            hook(*args, **kwargs)
        except Exception as error:
            raise ModelError(
                f"{self!r}: {getattr(hook, '__name__', 'a hook')} raised {error!r}",
                peripheral=self.__peripheral,
            ) from error

    @override
    def __repr__(self) -> str:
        where = f" on {self.__peripheral}" if self.__peripheral else ""
        return f"<{type(self).__qualname__}{where}>"
