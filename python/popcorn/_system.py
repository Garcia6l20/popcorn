"""Several machines on one virtual clock, their UARTs linked machine to machine."""

from __future__ import annotations

import asyncio
import contextlib
import weakref
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from types import TracebackType
from typing import TYPE_CHECKING, Any, Literal, cast

from popcorn import _core
from popcorn._enums import StopReason
from popcorn._errors import ConfigError, EmulationError, MachineBusy, MachineRunning
from popcorn._host import awaited
from popcorn._stop import Stop, stop_of
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine

_STOP_BITS = {0.5: 1, 1.0: 2, 1.5: 3, 2.0: 4}
_PARITY = {"none": 0, "even": 1, "odd": 2}


@dataclass(frozen=True)
class UartLink:
    """A UART link between two members of a `LinkedSystem`, both directions.

    A frame whose stop bit ends at the sender's system time t ends at the receiver at exactly
    t + `latency`, whatever the slicing, the thread timing or the system's quantum.

    Attributes:
        a: One end, `"<member>.<UART>"`, such as `"controller.UART4"`.
        b: The other end.
        latency: The line's latency L, above 0. No default: the system's quantum is the
            smallest L, so L is part of every link's declaration.
        baud: The line's declared baud: the shape of injected frames, and the baud a sender
            whose frames differ by more than 3 percent is warned about.
        data_bits: Declared data bits, 7, 8 or 9, at most 8 with a parity bit: a frame
            carries data bits plus the parity bit in at most 9 bits.
        stop_bits: Declared stop bits, 0.5, 1, 1.5 or 2.
        parity: Declared parity, "none", "even" or "odd". A sender whose USART frames with
            another parity, CR1.PCE and PS, is warned about as for another baud. Parity bits
            are not computed or checked, `uart-parity-pce` known issue: the warning is the
            whole effect.
    """

    a: str
    b: str
    latency: VirtualDuration
    baud: int
    data_bits: int = 8
    stop_bits: float = 1.0
    parity: Literal["none", "even", "odd"] = "none"


@dataclass(frozen=True)
class SystemStop:
    """Why and where a `LinkedSystem.run` ended.

    Attributes:
        reason: TIME when every member reached the target, IDLE when every member slept with
            nothing due and every link was empty, or the reason of the member stop that ended
            the run.
        member: The member whose stop ended the run, None for TIME and IDLE.
        stop: That member's stop, None for TIME and IDLE.
        time: System time of the stop: the target for TIME, the member's stop point for a
            member stop, the point where every member was quiescent for IDLE.
        stops: Each member's own stop at the end of the run, keyed by name. For TIME and IDLE
            every member's, a TIME or IDLE stop at its position. For a member stop only that
            member's: the others stand where the run left them, a position that depends on the
            quantum.
    """

    reason: StopReason
    member: str | None
    stop: Stop | None
    time: VirtualTime
    stops: Mapping[str, Stop] = field(default_factory=dict[str, Stop])


@dataclass(frozen=True)
class _Pause:
    reason: _core.PauseReason
    member: int | None
    at: int
    stop: Stop
    error: BaseException | None
    windows: int
    slices: int

    @staticmethod
    def of(raw: object) -> _Pause:
        reason, member, at, info, error, windows, slices = cast(
            tuple[
                _core.PauseReason,
                int | None,
                int,
                _core.StopInfo,
                BaseException | None,
                int,
                int,
            ],
            raw,
        )
        return _Pause(reason, member, at, stop_of(info), error, windows, slices)


class LinkedSystem:
    """Several machines, its members, on one virtual clock, their UARTs linked by `UartLink`s.

        async with asyncio.timeout(10):
            stop = await system.run(for_=ms(100))

    Each member runs on its own machine thread. Members meet at a barrier every quantum Q, the
    smallest link latency, at system times k Q from system start, and every frame reaches its
    receiver at exactly its sender's stop-bit end plus the link's latency. Results depend only
    on the members' inputs and the latencies, never on Q, the slicing, the wall timing or the
    threads.

    A member's local time, `Machine.time`, counts from its own reset. Its system time is its
    local time plus its base, fixed at construction by `offsets` and moved by a reset of the
    member, so system time runs on unchanged across it.

    A member while the system exists: `run`, `start`, `snapshot`, `restore` and `reset` of the
    member raise MachineBusy, sync accessors raise MachineRunning while the system runs and are
    allowed while it is stopped. The system owns the members' UARTs named in links: frames each
    transmits still reach its own `port`.

    The system holds its members until `close()`, also run by `with`.
    """

    __slots__ = (
        "__weakref__",
        "_closed",
        "_failed",
        "_flight",
        "_handle",
        "_index",
        "_machines",
        "_names",
        "_origin",
        "_ownerless",
        "_pausing",
        "_target",
    )

    def __init__(
        self,
        members: Mapping[str, Machine],
        links: Sequence[UartLink],
        *,
        offsets: Mapping[str, VirtualDuration] | None = None,
        on_member_reset: Literal["reset", "stop"] = "reset",
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
    ) -> None:
        """Link `members` by `links` and fix each member's system time.

        Args:
            members: The members by name, each the `Machine` of that name, parked.
            links: The links, at least one.
            offsets: System time of each member's current local time, 0 for an absent one, at
                least 0. A member booted to `main` at 408 us of its own time, with offset 0,
                stands at system time 0: its `Machine.time` stays 408 us ahead of
                `time_of(name)` until its next reset. Tap times and `SystemStop.time` are
                system times, `Stop.time` is the member's own.
            on_member_reset: What a firmware or watchdog reset of a member that runs its core
                does: "reset" resets it and runs on, "stop" ends the run with RESET_REQUEST.
            slice_instructions: Instructions after which a member's slice serves its machine's
                requests, above 0.
            slice_wall_budget: Wall seconds after which it does, above 0.

        Raises:
            ConfigError: A member named apart from its key, `members.<name>`, already in a
                linked system, or no link, `links`, or a link end that is not a UART of a
                member, a UART in two links or bridged to a pty, `links[i].a` or `links[i].b`,
                a value out of range, `links[i].latency`, `.baud`, `.data_bits`, `.stop_bits`
                or `.parity`, or an offset of no member or below 0, `offsets.<name>`.
            MachineBusy: A member is busy.
            ValueError: `slice_instructions` or `slice_wall_budget` not above 0.
        """
        if slice_instructions <= 0 or not slice_wall_budget > 0:
            raise ValueError("slice_instructions and slice_wall_budget must be above 0")
        if on_member_reset not in ("reset", "stop"):
            raise ConfigError(
                f"on_member_reset: {on_member_reset!r} is neither 'reset' nor 'stop'",
                key="on_member_reset",
                expected="'reset' or 'stop'",
            )
        self._closed = True
        self._names = list(members)
        self._machines = dict(members)
        self._index = {name: index for index, name in enumerate(self._names)}
        for name, machine in self._machines.items():
            _check_member(name, machine)
        if not links:
            raise ConfigError("a linked system needs a link", key="links", expected="a link")
        offset_ps = _offsets(self._names, offsets or {})
        self._handle = _core.SystemHandle(
            slice_instructions, max(1, round(slice_wall_budget * 1e9))
        )
        for name in self._names:
            native = self._machines[name]._handle  # pyright: ignore[reportPrivateUsage]
            self._handle.add_member(native, name, on_member_reset == "stop")
        used: set[tuple[str, str]] = set()
        for index, link in enumerate(links):
            self._add_link(index, link, used)
        self._handle.start(offset_ps)
        self._origin = max(offset_ps)
        self._closed = False
        self._failed: BaseException | None = None
        self._flight: asyncio.Future[_Pause] | None = None
        self._pausing = False
        self._target: int | None = None
        self._ownerless: set[int] = set()
        for machine in self._machines.values():
            machine._system = cast(Any, weakref.ref(self))  # pyright: ignore[reportPrivateUsage]

    def _add_link(self, index: int, link: UartLink, used: set[tuple[str, str]]) -> None:
        latency = cast(object, link.latency)
        if not isinstance(latency, VirtualDuration) or latency.ps <= 0:
            raise ConfigError(
                f"links[{index}].latency: {link.latency!r} is no duration above 0",
                key=f"links[{index}].latency",
                expected="a VirtualDuration above 0",
            )
        parity = cast(object, link.parity)
        word_bits = link.data_bits + (0 if parity == "none" else 1)
        for name, refused, expected in (
            ("baud", link.baud <= 0, "a baud above 0"),
            ("parity", parity not in _PARITY, "'none', 'even' or 'odd'"),
            (
                "data_bits",
                word_bits not in (7, 8, 9),
                "7 to 9 data bits, the parity bit included",
            ),
            ("stop_bits", link.stop_bits not in _STOP_BITS, "0.5, 1, 1.5 or 2 stop bits"),
        ):
            if refused:
                key = f"links[{index}].{name}"
                raise ConfigError(
                    f"{key}: {getattr(link, name)!r} is no UART line value",
                    key=key,
                    expected=expected,
                )
        ends = [self._end(index, side, end, used) for side, end in (("a", link.a), ("b", link.b))]
        for (sender, sender_link, _), (receiver, receiver_link, unit) in (
            (ends[0], ends[1]),
            (ends[1], ends[0]),
        ):
            spec = _core.WireSpec()
            spec.sender = sender
            spec.receiver = receiver
            spec.receiver_unit = unit
            spec.latency_ps = link.latency.ps
            spec.baud = link.baud
            spec.word_bits = word_bits
            spec.half_stops = _STOP_BITS[link.stop_bits]
            spec.parity = _PARITY[link.parity]
            spec.strict = self._machines[
                self._names[receiver]
            ]._strict_peripherals  # pyright: ignore[reportPrivateUsage]
            self._handle.add_wire(spec, sender_link, receiver_link)

    def _end(
        self, index: int, side: str, end: str, used: set[tuple[str, str]]
    ) -> tuple[int, _core.UartLink, str]:
        key = f"links[{index}].{side}"
        name, dot, unit = end.partition(".")
        machine = self._machines.get(name)
        if not dot or machine is None or not unit:
            raise ConfigError(
                f"{key}: {end!r} names no member UART", key=key, expected="'<member>.<UART>'"
            )
        ports = machine._ports  # pyright: ignore[reportPrivateUsage]
        try:
            native = ports.link(unit)
        except Exception as error:
            raise ConfigError(
                f"{key}: {end!r} is not a UART of {name}: {error}",
                key=key,
                expected="a UART of the member",
            ) from error
        serving = ports.serving(unit)
        if (name, serving) in used:
            raise ConfigError(
                f"{key}: {name}.{serving} is in two links", key=key, expected="a free UART"
            )
        if serving in ports.bridges:
            raise ConfigError(
                f"{key}: {name}.{serving} is bridged to a pty", key=key, expected="a free UART"
            )
        used.add((name, serving))
        return self._index[name], native, serving

    @property
    def members(self) -> Mapping[str, Machine]:
        """The members by name, in member order."""
        return dict(self._machines)

    @property
    def quantum(self) -> VirtualDuration:
        """The quantum Q, the smallest link latency: members meet every Q of system time."""
        return VirtualDuration(self._handle.quantum_ps())

    @property
    def time(self) -> VirtualTime:
        """The first barrier not exchanged yet, system time. Bookkeeping: it depends on the
        quantum, results do not. Members stand at their own times, `time_of`.

        Raises:
            MachineRunning: The system runs.
        """
        self._require_stopped()
        return VirtualTime(self._handle.pending_barrier_ps())

    def time_of(self, member: str) -> VirtualTime:
        """System time of `member`, its local time plus its base.

        Raises:
            KeyError: No such member.
            MachineRunning: The system runs.
        """
        index = self._index[member]
        self._require_stopped()
        return VirtualTime(self._handle.time_of_ps(index))

    async def run(
        self, *, for_: VirtualDuration | None = None, until: VirtualTime | None = None
    ) -> SystemStop:
        """Run every member's core to system time `until`, or `for_` past the last target.

        `for_` counts from the target of the previous TIME stop, else from the largest offset,
        the latest member's system time at construction, so ten `run(for_=ms(10))` equal one
        `run(for_=ms(100))`. Never from where a member stop left the members: that position
        depends on the quantum. Each member stops
        at the first block boundary at or after the target, an idle or halted one exactly at it.
        A member stop on the way, FAULT, a breakpoint or a reset under `on_member_reset="stop"`,
        ends the run there: the member waits at its stop, the others where the run left them,
        and the next `run` releases it.

        Cancelling the awaiting task pauses the system at its next barrier.

        Raises:
            TypeError: Neither or both of `for_` and `until`, or a value of another type.
            ValueError: `for_` negative, or `until` before the last target.
            MachineBusy: The system runs.
            EmulationError: The system was closed, or stopped on an error before.
            BaseException: The error that stopped the system, a link error or a member's
                handler error, of its own type. An error a member raised carries the note
                "raised by member <name> at system time <t>".
        """
        _check_target(for_, until)
        self._require_usable()
        await self._settled()
        if self._flight is not None:
            raise MachineBusy("the linked system runs")
        target = self._target_of(for_, until)
        for index in sorted(self._ownerless):
            self._handle.release(index)
        self._ownerless.clear()
        pause = await self._resume(target)
        return self._system_stop(pause, target)

    def _target_of(self, for_: VirtualDuration | None, until: VirtualTime | None) -> int:
        if for_ is not None:
            base = self._origin if self._target is None else self._target
            return base + for_.ps
        assert until is not None
        if self._target is not None and until.ps < self._target:
            raise ValueError(f"until {until} is before the last target {VirtualTime(self._target)}")
        return until.ps

    async def _settled(self) -> None:
        flight = self._flight
        if flight is None or not self._pausing:
            return
        with contextlib.suppress(Exception):
            await asyncio.shield(flight)

    async def _resume(self, until_ps: int | None) -> _Pause:
        future = self._handle.resume(until_ps)
        flight = asyncio.ensure_future(self._flown(future, until_ps))
        self._flight = flight
        try:
            return await asyncio.shield(flight)
        except asyncio.CancelledError:
            if not flight.done():
                self._pausing = True
                self._handle.request_pause()
            raise

    async def _flown(self, future: _core.HostFuture, until_ps: int | None) -> _Pause:
        try:
            pause = await awaited(future, _Pause.of, cancel=False)
        except asyncio.CancelledError:
            self._handle.request_pause()
            pause = _Pause.of(future.result(None))
        finally:
            self._flight = None
            self._pausing = False
        self._paused(pause, until_ps)
        return pause

    def _paused(self, pause: _Pause, until_ps: int | None) -> None:
        if pause.reason is _core.PauseReason.system_time and until_ps is not None:
            self._target = until_ps
        elif pause.reason is _core.PauseReason.delivered and pause.member is not None:
            self._ownerless.add(pause.member)
        elif pause.reason is _core.PauseReason.error:
            self._failed = pause.error
            if pause.error is not None and pause.member is not None:
                note = (
                    f"raised by member {self._names[pause.member]} at system time "
                    f"{VirtualTime(pause.at)}"
                )
                pause.error.add_note(note)

    def _system_stop(self, pause: _Pause, target: int) -> SystemStop:
        reason = pause.reason
        if reason is _core.PauseReason.error:
            assert pause.error is not None
            raise pause.error
        if reason is _core.PauseReason.closed:
            raise EmulationError("the linked system was closed")
        if reason is _core.PauseReason.delivered and pause.member is not None:
            name = self._names[pause.member]
            return SystemStop(
                pause.stop.reason, name, pause.stop, VirtualTime(pause.at), {name: pause.stop}
            )
        if reason is _core.PauseReason.idle:
            return SystemStop(
                StopReason.IDLE, None, None, VirtualTime(pause.at), self._stops(StopReason.IDLE)
            )
        if reason is _core.PauseReason.requested:
            return SystemStop(
                StopReason.USER, None, None, VirtualTime(pause.at), self._stops(StopReason.USER)
            )
        return SystemStop(
            StopReason.TIME, None, None, VirtualTime(target), self._stops(StopReason.TIME)
        )

    def _stops(self, reason: StopReason) -> dict[str, Stop]:
        stops: dict[str, Stop] = {}
        for name, machine in self._machines.items():
            pc = machine.pc
            stops[name] = Stop(
                reason=reason,
                pc=pc,
                location=machine.locate(pc),
                instructions=machine.instructions,
                time=machine.time,
            )
        return stops

    def close(self) -> None:
        """End the system: stop it at its last pause point, unlink the members and give their
        UARTs back. The members then run alone, each at its own time. Idempotent.

        Raises:
            RuntimeError: Called from a callback of a machine.
        """
        if self._closed:
            return
        self._handle.check_caller()
        self._closed = True
        if self._flight is None and self._failed is None:
            with contextlib.suppress(RuntimeError):
                self._handle.finish()
        self._handle.close()
        for machine in self._machines.values():
            machine._system = None  # pyright: ignore[reportPrivateUsage]

    def __enter__(self) -> LinkedSystem:
        return self

    def __exit__(
        self,
        kind: type[BaseException] | None,
        error: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    def _require_usable(self) -> None:
        if self._closed:
            raise EmulationError("the linked system was closed")
        if self._failed is not None:
            raise EmulationError(
                f"the linked system stopped on an error: {self._failed!r}"
            ) from self._failed

    def _require_stopped(self) -> None:
        if self._flight is not None:
            raise MachineRunning("the linked system runs")

    def __repr__(self) -> str:
        return f"LinkedSystem({', '.join(self._names)})"


def _check_member(name: str, machine: Machine) -> None:
    if machine.name != name:
        raise ConfigError(
            f"members.{name}: the machine is named {machine.name!r}",
            key=f"members.{name}",
            expected=f"a Machine named {name!r}",
        )
    owner = machine._system  # pyright: ignore[reportPrivateUsage]
    if owner is not None and owner() is not None:
        raise ConfigError(
            f"members.{name}: the machine is a member of another linked system",
            key=f"members.{name}",
            expected="a machine in no linked system",
        )
    if machine.busy:
        raise MachineBusy(f"members.{name}: the machine is busy")


def _check_target(for_: VirtualDuration | None, until: VirtualTime | None) -> None:
    if (for_ is None) == (until is None):
        raise TypeError("run takes for_ or until")
    if for_ is not None:
        if not isinstance(cast(object, for_), VirtualDuration):
            raise TypeError(f"for_ must be a VirtualDuration, not {type(for_).__name__}")
        if for_.ps < 0:
            raise ValueError("for_ must not be negative")
    elif not isinstance(cast(object, until), VirtualTime):
        raise TypeError(f"until must be a VirtualTime, not {type(until).__name__}")


def _offsets(names: Sequence[str], offsets: Mapping[str, VirtualDuration]) -> list[int]:
    for name, given in offsets.items():
        offset = cast(object, given)
        if name not in names:
            raise ConfigError(
                f"offsets.{name}: no such member", key=f"offsets.{name}", expected="a member name"
            )
        if not isinstance(offset, VirtualDuration) or offset.ps < 0:
            raise ConfigError(
                f"offsets.{name}: {offset!r} is no duration of at least 0",
                key=f"offsets.{name}",
                expected="a VirtualDuration of at least 0",
            )
    return [offsets[name].ps if name in offsets else 0 for name in names]
