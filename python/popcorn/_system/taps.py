from __future__ import annotations

import heapq
import threading
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, cast, override

from popcorn import _core
from popcorn._enums import I2CPhase
from popcorn._errors import (
    MachineRunning,
)
from popcorn._system.checks import check_count, check_time, data_frames, line_low
from popcorn._system.links import LineState
from popcorn._system.wiring import I2CWired, LinkWires, SpiWired
from popcorn._tap import TapFrame
from popcorn.units import VirtualTime

if TYPE_CHECKING:
    from popcorn._system.system import System

_TAP_KEEP = 1 << 20
_OPEN_END = (1 << 63) - 1


@dataclass(frozen=True)
class SpiTapFrame:
    """One frame on an SPI bus, as `SpiTap` shows it. Times are system times.

    Attributes:
        slave: The slave's SPI, `"<member>.<SPI>"`.
        cs: The chip select pin of the master that selected the slave, such as `"PA4"`, None for
            the master's SS output.
        unit: Number the master's side gave the frame on this slave's link, increasing per
            slave: chip select edges of that slave take numbers from the same counter.
        at: End of the master's frame at the master: the stamp the slave sees `latency` later.
        reply: Stamp of the slave's reply, `at` + `latency` + one frame time, or the time the
            frame was dropped. The master completes the frame `latency` after `reply`.
        mosi: The frame the master sent, in wire order, first bit on the wire in bit `bits - 1`.
        miso: The frame the slave returned, in wire order, all ones when `dropped`.
        bits: Frame size, 4 to 32.
        dropped: Whether the slave never clocked the frame, a reset of the slave dropped it or a
            deselect cut it, and the master got all ones.
    """

    slave: str
    cs: str | None
    unit: int
    at: VirtualTime
    reply: VirtualTime
    mosi: int
    miso: int
    bits: int
    dropped: bool


@dataclass(frozen=True)
class I2CTapPhase:
    """One phase on an I2C bus between the controller and one target, as `I2CTap` shows it.
    Times are system times. An address phase gives one record per target. A write or read phase
    gives one per target that acknowledged the transfer's last address.

    Attributes:
        target: The target's I2C, `"<member>.<I2C>"`.
        unit: Number the controller's side gave the phase on this target's link, increasing per
            target.
        phase: `I2CPhase.ADDRESS`, `WRITE` or `READ`.
        byte: For an address phase the address byte, the 7-bit address in bits 7 to 1 and the
            read bit in bit 0. For a write phase the byte the controller wrote. For a read phase
            the byte the target sent, 0xFF when `dropped`.
        ack: The target's acknowledge of an address or a written byte, the controller's
            acknowledge of a read byte.
        at: End of the controller's phase at the controller: the stamp the target sees `latency`
            later.
        answer: Stamp of the target's answer, `at` + `latency` + the phase time + the target's
            stretch, or the time the phase was dropped. The controller completes the phase
            `latency` after the last target's answer.
        dropped: Whether the target never answered the phase, a reset of the target dropped it
            or a STOP cut it, and the controller got NACK or 0xFF.
    """

    target: str
    unit: int
    phase: I2CPhase
    byte: int
    ack: bool
    at: VirtualTime
    answer: VirtualTime
    dropped: bool


class UartTap:
    """Both directions of one link of a `System`, `system.tap("a.USART1")`: every frame
    on the line with its system times, and frames injected, replaced or dropped by number.

    The tap gets a frame once it ended. After a member stop, an IDLE pause or an error it holds
    every frame whose stop-bit `end` is at or before the pause point and no later frame,
    whatever the quantum, the slicing or the wall timing. After a TIME stop or a requested pause
    it holds the frames ended by the earliest member's position, which can lie before the pause
    point by an amount that depends on the quantum. After `close()` it holds every frame that
    ended by the last pause. While the system runs it can lag the latest frames by up to two
    windows of the quantum behind the receiving member's time, and a read at a pause is complete
    as above.
    It keeps up to 1 Mi frames per direction not read yet, about 380 bytes each, the oldest
    dropped and counted in `dropped`: take them with `frames()`.
    Reading never changes what the receiver gets.

    `towards` names the receiving end of a direction, `"<member>.<UART>"`.

    Line cuts, `cut`, `break_line` and `restore_line`, pull the cable over a window [start,
    end) of system time at the senders, the base of `TapFrame.start`, both directions: each
    receiver's line is cut over [start + L, end + L), L the link's latency. Per direction:

    - A frame ending at or before `start`, or starting at or after `end`: delivered as sent.
    - A frame starting before `start` and ending after it: delivered, cut at `start + L`,
      tapped `cut` and `lost` with its stop-bit `end`. On an idle line the receiver gets it as
      an aborted frame: nothing in RDR, IDLE one receiver frame after `start + L`. A frame
      both aborted by its sender and cut keeps the abort's `end`, and the receiver sees the
      earlier of the two.
    - A frame starting inside the cut: never reaches the receiver, tapped `cut`, and also
      `lost` when it ends after `end`.
    - A frame an armed `drop` keeps off the line: `dropped` only.

    An injected frame is classified the same way on its receiver times against [start + L,
    end + L). Deviations: silicon samples a frame cut at `start + L`, which an idle line can
    turn into a clean wrong byte, and can take the tail of a frame crossing `end + L` for a
    start bit. The model delivers nothing for either.

    `line="idle"`, the default, keeps the receiver's line high over the cut. `line="break"`
    holds it low, RM0456 66.5.7: one break character, FE with data 0 and RXFNE, at the
    receiver's stop-bit sample of a character starting at `start + L`, then nothing until the
    line rises, and IDLE one receiver frame after `end + L` unless a frame starts before. A
    frame whose stop bit is not sampled yet at `start + L` becomes that break character at its
    own stop-bit sample, data 0, where silicon keeps the bits received before the cut. A cut
    shorter than a frame gives the sampled byte, with FE only when the stop-bit sample falls
    inside it, and nothing when it ends before the start bit's middle sample. LBDF is never set.

    Errors ignore cuts: shape and overlap errors are found as without them. An injection over a
    peer frame raises the overlap error, or is refused stopped, even when a cut keeps both off
    the receiver's line, and a cut never turns a refused stopped injection into an accepted
    one.

    Who changes cuts, and the time "now" that `break_line` and `restore_line` take:

    - Stopped: no run, no free run, no member goal and no member `running()` pending. Anyone
      but a callback of another machine, see the last item. Now is the later of the two ends'
      system times, `System.time_of`. After a run a member stop ended at P, a member that runs
      a goal and sleeps below P stands at P. A member that ran past P stands where the run left
      it, a position that depends on the quantum, and so does now. `cut` accepts a start from
      now on.
    - While the system runs, between two pauses of `running()` too, or with a member goal
      pending: a callback of the link's cut owner, `owner`, such as an intercept or a model
      method on that member. Now is the owner's system time at the callback, so the result is
      the same in every run, at any quantum and slicing. `cut` there accepts a start from that
      time on.
    - From the `running()` body or another thread while the system runs, or a member goal is
      pending: `break_line` and `restore_line` raise RuntimeError. `cut` accepts a start at or
      after the reach of the link's deliveries so far, or from now when it finds the system
      paused: its acceptance depends on wall timing, its effect only on `start` and `end`. A
      body `cut` while the owner's callbacks also change cuts runs in wall order with them, so
      which call a refusal hits depends on wall timing.
    - A callback of any machine other than `owner`, a member or a machine outside this system:
      `cut`, `break_line` and `restore_line` raise RuntimeError, stopped or running, before any
      change.

    Cuts never overlap, touching allowed, and at most 65536 wait per link, passed ones
    included until they are erased, about one quantum after their end plus L and the end of
    the last frame they touched. Snapshots keep the cuts. `owner` is harness configuration: not in
    snapshots, kept across `System.restore`.
    """

    __slots__ = (
        "__weakref__",
        "_buffers",
        "_dropped_base",
        "_ends",
        "_link",
        "_lock",
        "_overflow",
        "_system",
        "_wires",
    )

    def __init__(self, system: System, wired: LinkWires) -> None:
        """Wrap a link of `system`. Use `System.tap` instead."""
        self._system = system
        self._ends = wired.ends
        self._wires = wired.wires
        self._link = wired.link
        self._buffers = (deque[TapFrame](maxlen=_TAP_KEEP), deque[TapFrame](maxlen=_TAP_KEEP))
        self._overflow = 0
        self._dropped_base = 0
        self._lock = threading.Lock()

    @property
    def ends(self) -> tuple[str, str]:
        """The link's two ends, `"<member>.<UART>"`, each named by the UART serving it."""
        return self._ends

    @property
    def available(self) -> int:
        """Frames the tap holds, both directions, not read yet."""
        with self._lock:
            self._pull()
            return len(self._buffers[0]) + len(self._buffers[1])

    @property
    def dropped(self) -> int:
        """Frames the tap dropped, both directions, because it held its limit, since system
        start or the last `System.restore`."""
        return self._dropped_total() - self._dropped_base

    def _dropped_total(self) -> int:
        handle = self._system._handle  # pyright: ignore[reportPrivateUsage]
        return self._overflow + sum(handle.tap_dropped(wire) for wire in self._wires)

    def frames(self, *, towards: str | None = None) -> list[TapFrame]:
        """Take every frame the tap holds, towards `towards` only when given, oldest first.
        Never runs the system, allowed stopped and running, from any thread.

        After a member stop, an IDLE pause, an error or `close()`, it takes exactly the frames
        ended by that point and not taken yet, whatever the quantum, the slicing or the wall
        timing. After a TIME stop or a requested pause it takes those ended by the earliest
        member's position, see the class. While the system runs, the tap fills at each
        barrier and holds every frame ended two windows before the receiving member's time, so
        how many frames a call takes depends on wall timing. Each frame's content never does.
        Before a free run sleeps, it holds every frame ended by the earliest member's position.

        Returns:
            The frames, ordered by `end`, then direction: towards the link's second end first.

        Raises:
            TypeError: `towards` is no str.
            ValueError: `towards` is no end of this link.
        """
        directions = (0, 1) if towards is None else (self._direction(towards),)
        taken: list[TapFrame] = []
        with self._lock:
            self._pull()
            for direction in directions:
                buffer = self._buffers[direction]
                taken.extend(buffer)
                buffer.clear()
        if towards is None:
            taken.sort(key=lambda frame: (frame.end.ps, frame.towards != self._ends[1]))
        return taken

    def inject(self, data: bytes, *, at: VirtualTime, towards: str) -> None:
        """Put frames on the line towards `towards`, back to back at the link's declared baud
        and shape, as if a device on the line sent them. `at` is the start-bit edge of the first
        frame at the receiver, system time. Delivered at exactly their times, whatever the
        quantum, the slicing or the wall timing. With a declared parity, each frame carries
        its parity bit in the MSB of its word, in place of that data bit.

        Stopped or paused, they are checked at the call: `at` must be at or after the
        receiver's system time, and no frame may overlap one already on the receiver's line.
        While the system runs, from a `running()` body or another thread, `at` must be at or
        after the link's latency and at or after the reach of the deliveries done so far, which
        depends on wall timing, and an overlap is found later: the system stops with an error at
        a time fixed by the frames, the later frame's start for two injected frames, and the
        later of the injected frame's start and the peer frame's sender start for an injected
        frame and a peer frame. The error is the receiving member's.

        Raises:
            TypeError: `data` is no bytes, `at` no VirtualTime or `towards` no str.
            ValueError: `data` empty, `towards` no end of this link, `at` too early, or, stopped
                or paused, an overlap. The message names the bound.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine.
        """
        frames = data_frames(data)
        if not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"at must be a VirtualTime, not {type(at).__name__}")
        direction = self._direction(towards)
        system = self._system
        system._require_usable()  # pyright: ignore[reportPrivateUsage]
        wire = self._wires[direction]
        paused = system._flight is None  # pyright: ignore[reportPrivateUsage]
        try:
            refused = self._native.inject(wire, frames, at.ps, paused)
        except MachineRunning:
            if not paused:
                raise
            refused = self._native.inject(wire, frames, at.ps, False)
        if refused is None:
            return
        reason, bound_ps = refused
        bound = VirtualTime(bound_ps)
        receiver = self._ends[1 - direction]
        if reason == "before_receiver":
            raise ValueError(f"inject at {at}: before {receiver}'s system time {bound}")
        if reason == "before_drained":
            raise ValueError(
                f"inject at {at}: before {bound}, the earliest time the running system can"
                " still deliver"
            )
        raise ValueError(
            f"inject at {at}: overlaps the frame starting at {bound} on {receiver}'s line"
        )

    def cut(self, start: VirtualTime, end: VirtualTime, *, line: LineState = "idle") -> None:
        """Cut the link over [`start`, `end`) of system time, both directions, the line idle or
        held low over the cut as `line` says: see the class doc for what each frame and the
        receiver then get, and who may call it with which earliest `start`.

        A `line="break"` cut keeps `System.run` without `until` from pausing IDLE, and a free
        run from sleeping, until its release is handed and the receiver's IDLE one frame after
        `end + L` has fired, since the break's events are due until then. An open break cut
        whose line fell already does not.

        Raises:
            TypeError: `start` or `end` is no VirtualTime, a VirtualDuration included.
            ValueError: `end` not after `start`, `line` neither "idle" nor "break", `start`
                before the earliest start accepted now, the cut overlaps a cut registered
                before, or 65536 cuts wait. The message names the bound or the cut overlapped.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine other than `owner`, stopped or
                running.
            MachineRunning: Called from inside a parked section.
        """
        check_time("start", start)
        check_time("end", end)
        if end.ps <= start.ps:
            raise ValueError(f"cut [{start}, {end}): end must be after start")
        low = line_low(line)
        system = self._system
        system._require_usable()  # pyright: ignore[reportPrivateUsage]
        stopped = system._flight is None  # pyright: ignore[reportPrivateUsage]
        try:
            outcome = self._native.cut(self._link, start.ps, end.ps, low, stopped)
        except MachineRunning:
            if not stopped:
                raise
            outcome = self._native.cut(self._link, start.ps, end.ps, low, False)
        self._changed(outcome, f"cut [{start}, {end})")

    def break_line(self, *, line: LineState = "idle") -> VirtualTime:
        """Cut the link from now until `restore_line`, both directions, the line idle or held
        low as `line` says. Now is the later end's system time when stopped, the owner's system
        time from the owner's callback while the system runs, see the class doc.

        Returns:
            The cut's start, now.

        Raises:
            TypeError: `line` is no str.
            ValueError: `line` neither "idle" nor "break", a cut is open already, a cut
                registered before lies after now, or 65536 cuts wait.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine other than `owner`, stopped or
                running. Called while the system runs, or a member goal is pending, from the
                `running()` body or another thread. Use a callback of `owner`, or
                `cut(start, end)`.
            MachineRunning: Called from inside a parked section.
        """
        low = line_low(line)
        system = self._system
        system._require_usable()  # pyright: ignore[reportPrivateUsage]
        stopped = system._quiet()  # pyright: ignore[reportPrivateUsage]
        return self._changed(self._native.break_line(self._link, low, stopped), "break_line")

    def restore_line(self) -> VirtualTime:
        """End the open cut now, both directions, now taken as `break_line` takes it. A cut
        opened at or after now is withdrawn whole.

        Returns:
            The cut's end, now.

        Raises:
            ValueError: No cut is open.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: As `break_line`.
            MachineRunning: Called from inside a parked section.
        """
        system = self._system
        system._require_usable()  # pyright: ignore[reportPrivateUsage]
        stopped = system._quiet()  # pyright: ignore[reportPrivateUsage]
        return self._changed(self._native.restore(self._link, stopped), "restore_line")

    @property
    def owner(self) -> str:
        """The member whose callbacks may change the link's cuts while the system runs, its
        cut owner. Default the member of the link's first end, `UartLink.a`. Set it while the
        system is stopped, to any member, an end of the link or not. It never depends on which
        member calls first. Reading it works from any callback of a member and raises
        `EmulationError` once the system is closed.

        Raises, when set:
            TypeError: The value is no str.
            ValueError: No member of the system has that name.
            EmulationError: The system was closed, or stopped on an error before.
            MachineRunning: The system runs, or a member goal is pending.
        """
        system = self._system
        return system._names[  # pyright: ignore[reportPrivateUsage]
            self._native.link_owner(self._link)
        ]

    @owner.setter
    def owner(self, member: str) -> None:
        if not isinstance(cast(object, member), str):
            raise TypeError(f"owner must be a str, not {type(member).__name__}")
        system = self._system
        index = system._index.get(member)  # pyright: ignore[reportPrivateUsage]
        if index is None:
            raise ValueError(f"owner {member!r} is no member of {system!r}")
        system._require_quiet("owner change")  # pyright: ignore[reportPrivateUsage]
        self._native.set_link_owner(self._link, index)

    def _changed(
        self, outcome: tuple[int | None, tuple[str, int, int] | None], what: str
    ) -> VirtualTime:
        at, refused = outcome
        if at is not None:
            return VirtualTime(at)
        assert refused is not None
        reason, bound_ps, end_ps = refused
        bound = VirtualTime(bound_ps)
        if reason == "before_bound":
            raise ValueError(f"{what}: starts before {bound}, the earliest start accepted now")
        if reason == "overlaps":
            if end_ps == _OPEN_END:
                raise ValueError(f"{what}: overlaps the cut open from {bound}")
            raise ValueError(f"{what}: overlaps the cut [{bound}, {VirtualTime(end_ps)})")
        if reason == "already_open":
            raise ValueError(f"{what}: the cut from {bound} is open, restore_line() it first")
        if reason == "none_open":
            raise ValueError(f"{what}: no cut is open")
        raise ValueError(f"{what}: {self._native.max_waiting_cuts()} cuts wait on the link already")

    def replace(self, index: int, data: bytes, *, towards: str) -> None:
        """Give the sender's frames numbered `index` on, one per byte, new data before they
        start: the receiver and the tap see the new data at the frames' own times, with the
        parity bit of the sender's own parity in the MSB of the word. Armed at any time, from
        a `running()` body too: while the sender runs, whether the edit comes before the frame
        starts depends on wall timing, its effect does not.

        Raises:
            TypeError: `index` is no int, `data` no bytes, or `towards` no str.
            ValueError: `index` negative, `data` empty, `towards` no end of this link, a frame
                it covers started already, `posted`, or it overlaps an edit armed before.
            EmulationError: The system was closed, or stopped on an error before.
        """
        frames = data_frames(data)
        self._edit(
            index, len(frames), towards, lambda wire: self._native.replace(wire, index, frames)
        )

    def drop(self, index: int, n: int = 1, *, towards: str) -> None:
        """Keep the sender's `n` frames numbered `index` on off the line: the sender sends
        them, the receiver sees an idle line, the tap shows them `dropped`. Armed as `replace`.

        Raises:
            TypeError: `index` or `n` is no int, or `towards` no str.
            ValueError: `index` negative, `n` not above 0, `towards` no end of this link, or as
                `replace`.
            EmulationError: The system was closed, or stopped on an error before.
        """
        check_count("n", n, 1)
        self._edit(index, n, towards, lambda wire: self._native.drop(wire, index, n))

    def posted(self, *, towards: str) -> int:
        """Frames the sender started towards `towards` since system start: the lowest index
        `replace` and `drop` still accept. While the sender runs it depends on wall timing.

        Raises:
            ValueError: `towards` is no end of this link.
        """
        return self._native.posted(self._wires[self._direction(towards)])

    @property
    def _native(self) -> _core.SystemHandle:
        return self._system._handle  # pyright: ignore[reportPrivateUsage]

    def _edit(self, index: int, n: int, towards: str, arm: Callable[[int], bool]) -> None:
        check_count("index", index, 0)
        direction = self._direction(towards)
        self._system._require_usable()  # pyright: ignore[reportPrivateUsage]
        wire = self._wires[direction]
        if arm(wire):
            return
        started = self._native.posted(wire)
        if index < started:
            raise ValueError(
                f"frame {index} towards {towards} started already: {started} frames started"
            )
        raise ValueError(
            f"frames {index} to {index + n - 1} towards {towards} overlap an edit armed before"
        )

    def _discard(self) -> None:
        """Drop every frame held, native and buffered, for a restore."""
        with self._lock:
            for wire in self._wires:
                self._native.tap_take(wire)
            for buffer in self._buffers:
                buffer.clear()
            self._dropped_base = self._dropped_total()

    def _direction(self, towards: str) -> int:
        if not isinstance(cast(object, towards), str):
            raise TypeError(f"towards must be a str, not {type(towards).__name__}")
        named = self._system._end_name(towards)  # pyright: ignore[reportPrivateUsage]
        if named == self._ends[1]:
            return 0
        if named == self._ends[0]:
            return 1
        raise ValueError(f"towards {towards!r} is neither {self._ends[0]} nor {self._ends[1]}")

    def _pull(self) -> None:
        """Move the native taps' frames into the buffers, with `_lock` held: two pulls at once
        would append one direction's frames out of order."""
        handle = self._native
        for direction, wire in enumerate(self._wires):
            towards = self._ends[1 - direction]
            buffer = self._buffers[direction]
            records = handle.tap_take(wire)
            self._overflow += max(0, len(buffer) + len(records) - _TAP_KEEP)
            buffer.extend(
                TapFrame(
                    data=record.data,
                    index=record.index,
                    start=VirtualTime(record.start_ps),
                    end=VirtualTime(record.end_ps),
                    arrival=VirtualTime(record.arrival_ps),
                    towards=towards,
                    injected=record.injected,
                    brk=record.brk,
                    lost=record.lost,
                    dropped=record.dropped,
                    cut=record.cut,
                )
                for record in records
            )

    @override
    def __repr__(self) -> str:
        return f"UartTap({self._ends[0]} <-> {self._ends[1]})"


class SpiTap:
    """Every frame on one `SpiBus` of a `System`, `system.tap("a.SPI1", SpiTap)`, with its system
    times.

    The tap gets a frame when the master's side takes the slave's reply, in the master's drain
    that follows the reply's stamp. After a member stop, an IDLE pause, an error or a TIME stop
    it holds the frames whose reply the master's drains reached, which depends on the quantum
    for a frame whose reply lies near the pause point. After `close()` it holds every frame the
    master took. A frame's content never depends on the quantum, the slicing or the wall
    timing. It keeps up to 1 Mi frames not read yet, the oldest dropped and counted in
    `dropped`: take them with `frames()`. The frames one call takes are in the order of `at`, by
    slave index for frames of one `at`. Frames taken by separate calls can come out of order of
    `at`, since the master takes a slow frame's reply after a later fast one's. Reading never
    changes what the master or the slaves get.
    """

    __slots__ = (
        "__weakref__",
        "_buffer",
        "_bus",
        "_dropped_base",
        "_ends",
        "_lock",
        "_overflow",
        "_selects",
        "_system",
    )

    def __init__(self, system: System, wired: SpiWired) -> None:
        """Wrap a bus of `system`. Use `System.tap` instead."""
        self._system = system
        self._ends = wired.ends
        self._selects = wired.selects
        self._bus = wired.bus
        self._buffer = deque[SpiTapFrame](maxlen=_TAP_KEEP)
        self._overflow = 0
        self._dropped_base = 0
        self._lock = threading.Lock()

    @property
    def ends(self) -> tuple[str, ...]:
        """The bus's ends, `"<member>.<SPI>"`, each named by the SPI serving it: the master,
        then the slaves in the order of `SpiBus.slaves`."""
        return self._ends

    @property
    def available(self) -> int:
        """Frames the tap holds, not read yet."""
        with self._lock:
            self._pull()
            return len(self._buffer)

    @property
    def dropped(self) -> int:
        """Frames the tap dropped because it held its limit, since system start or the last
        `System.restore`."""
        return self._dropped_total() - self._dropped_base

    def _dropped_total(self) -> int:
        return self._overflow + self._native.spi_tap_dropped(self._bus)

    def frames(self, *, slave: str | None = None) -> list[SpiTapFrame]:
        """Take every frame the tap holds, of slave `slave` only when given, oldest first.
        Never runs the system, allowed stopped and running, from any thread.

        Args:
            slave: A slave's SPI, `"<member>.<SPI>"` or an alias of its SPI, None for all.

        Returns:
            The frames, in the order of `at`, by slave index for frames of one `at`.

        Raises:
            TypeError: `slave` is no str.
            ValueError: `slave` is no slave of this bus.
        """
        index = None if slave is None else self._slave(slave)
        with self._lock:
            self._pull()
            if index is None:
                taken = list(self._buffer)
                self._buffer.clear()
            else:
                named = self._ends[index + 1]
                taken = [frame for frame in self._buffer if frame.slave == named]
                kept = [frame for frame in self._buffer if frame.slave != named]
                self._buffer.clear()
                self._buffer.extend(kept)
        return taken

    @property
    def _native(self) -> _core.SystemHandle:
        return self._system._handle  # pyright: ignore[reportPrivateUsage]

    def _slave(self, slave: str) -> int:
        if not isinstance(cast(object, slave), str):
            raise TypeError(f"slave must be a str, not {type(slave).__name__}")
        named = self._system._end_name(slave)  # pyright: ignore[reportPrivateUsage]
        if named in self._ends[1:]:
            return self._ends.index(named) - 1
        raise ValueError(f"slave {slave!r} is no slave of {', '.join(self._ends[1:])}")

    def _discard(self) -> None:
        """Drop every frame held, native and buffered, for a restore."""
        with self._lock:
            self._native.spi_tap_take(self._bus)
            self._buffer.clear()
            self._dropped_base = self._dropped_total()

    def _pull(self) -> None:
        """Move the native tap's frames into the buffer, with `_lock` held."""
        records = sorted(
            self._native.spi_tap_take(self._bus), key=lambda record: (record.at_ps, record.slave)
        )
        self._overflow += max(0, len(self._buffer) + len(records) - _TAP_KEEP)
        pulled = [
            SpiTapFrame(
                slave=self._ends[record.slave + 1],
                cs=self._selects[record.slave],
                unit=record.unit,
                at=VirtualTime(record.at_ps),
                reply=VirtualTime(record.reply_ps),
                mosi=record.mosi,
                miso=record.miso,
                bits=record.bits,
                dropped=record.dropped,
            )
            for record in records
        ]
        self._buffer = _merged(self._buffer, pulled, self._order)

    def _order(self, frame: SpiTapFrame) -> tuple[int, int]:
        return frame.at.ps, self._ends.index(frame.slave)

    @override
    def __repr__(self) -> str:
        return f"SpiTap({self._ends[0]} -> {', '.join(self._ends[1:])})"


class I2CTap:
    """Every phase on one `I2CBus` of a `System`, `system.tap("a.I2C1", I2CTap)`, with its system
    times.

    The tap gets a phase when the controller's side takes the target's answer, in the
    controller's drain that follows the answer's stamp. After a member stop, an IDLE pause, an
    error or a TIME stop it holds the phases whose answer the controller's drains reached, which
    depends on the quantum for a phase whose answer lies near the pause point. After `close()`
    it holds every phase the controller took. A phase's content never depends on the quantum,
    the slicing or the wall timing. It keeps up to 1 Mi phases not read yet, the oldest dropped
    and counted in `dropped`: take them with `phases()`. The phases one call takes are in the
    order of `at`, by target index for phases of one `at`. Phases taken by separate calls can
    come out of the order of `at`: the controller can take the answer of a phase it gave up, by
    a reset or a PE clear, after the answer of a later phase. Reading never changes what the
    controller or the targets get.
    """

    __slots__ = (
        "__weakref__",
        "_buffer",
        "_bus",
        "_dropped_base",
        "_ends",
        "_lock",
        "_overflow",
        "_system",
    )

    def __init__(self, system: System, wired: I2CWired) -> None:
        """Wrap a bus of `system`. Use `System.tap` instead."""
        self._system = system
        self._ends = wired.ends
        self._bus = wired.bus
        self._buffer = deque[I2CTapPhase](maxlen=_TAP_KEEP)
        self._overflow = 0
        self._dropped_base = 0
        self._lock = threading.Lock()

    @property
    def ends(self) -> tuple[str, ...]:
        """The bus's ends, `"<member>.<I2C>"`, each named by the I2C serving it: the controller,
        then the targets in the order of `I2CBus.ports`."""
        return self._ends

    @property
    def available(self) -> int:
        """Phases the tap holds, not read yet."""
        with self._lock:
            self._pull()
            return len(self._buffer)

    @property
    def dropped(self) -> int:
        """Phases the tap dropped because it held its limit, since system start or the last
        `System.restore`."""
        return self._dropped_total() - self._dropped_base

    def _dropped_total(self) -> int:
        return self._overflow + self._native.i2c_tap_dropped(self._bus)

    def phases(self, *, target: str | None = None) -> list[I2CTapPhase]:
        """Take every phase the tap holds, of target `target` only when given, oldest first.
        Never runs the system, allowed stopped and running, from any thread.

        Args:
            target: A target's I2C, `"<member>.<I2C>"` or an alias of its I2C, None for all.

        Returns:
            The phases, in the order of `at`, by target index for phases of one `at`.

        Raises:
            TypeError: `target` is no str.
            ValueError: `target` is no target of this bus.
        """
        index = None if target is None else self._target(target)
        with self._lock:
            self._pull()
            if index is None:
                taken = list(self._buffer)
                self._buffer.clear()
            else:
                named = self._ends[index + 1]
                taken = [phase for phase in self._buffer if phase.target == named]
                kept = [phase for phase in self._buffer if phase.target != named]
                self._buffer.clear()
                self._buffer.extend(kept)
        return taken

    @property
    def _native(self) -> _core.SystemHandle:
        return self._system._handle  # pyright: ignore[reportPrivateUsage]

    def _target(self, target: str) -> int:
        if not isinstance(cast(object, target), str):
            raise TypeError(f"target must be a str, not {type(target).__name__}")
        named = self._system._end_name(target)  # pyright: ignore[reportPrivateUsage]
        if named in self._ends[1:]:
            return self._ends.index(named) - 1
        raise ValueError(f"target {target!r} is no target of {', '.join(self._ends[1:])}")

    def _discard(self) -> None:
        """Drop every phase held, native and buffered, for a restore."""
        with self._lock:
            self._native.i2c_tap_take(self._bus)
            self._buffer.clear()
            self._dropped_base = self._dropped_total()

    def _pull(self) -> None:
        """Move the native tap's phases into the buffer, with `_lock` held."""
        records = sorted(
            self._native.i2c_tap_take(self._bus), key=lambda record: (record.at_ps, record.target)
        )
        self._overflow += max(0, len(self._buffer) + len(records) - _TAP_KEEP)
        pulled = [
            I2CTapPhase(
                target=self._ends[record.target + 1],
                unit=record.unit,
                phase=I2CPhase(record.phase),
                byte=record.byte,
                ack=record.ack,
                at=VirtualTime(record.at_ps),
                answer=VirtualTime(record.answer_ps),
                dropped=record.dropped,
            )
            for record in records
        ]
        self._buffer = _merged(self._buffer, pulled, self._order)

    def _order(self, phase: I2CTapPhase) -> tuple[int, int]:
        return phase.at.ps, self._ends.index(phase.target)

    @override
    def __repr__(self) -> str:
        return f"I2CTap({self._ends[0]} -> {', '.join(self._ends[1:])})"


def _merged[T](
    buffer: deque[T], pulled: list[T], order: Callable[[T], tuple[int, int]]
) -> deque[T]:
    if buffer and pulled and order(buffer[-1]) > order(pulled[0]):
        return deque(heapq.merge(buffer, pulled, key=order), maxlen=_TAP_KEEP)
    buffer.extend(pulled)
    return buffer
