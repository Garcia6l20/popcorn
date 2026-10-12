"""Several machines on one virtual clock, their UARTs linked machine to machine."""

from __future__ import annotations

import asyncio
import concurrent.futures
import contextlib
import logging
import threading
import weakref
from collections import deque
from collections.abc import AsyncGenerator, Callable, Mapping, Sequence
from dataclasses import dataclass, field, replace
from types import TracebackType
from typing import TYPE_CHECKING, Any, Literal, cast

from popcorn import _core
from popcorn._enums import ResetKind, StopReason
from popcorn._errors import ConfigError, EmulationError, MachineBusy, MachineRunning
from popcorn._host import awaited, watch_awaits
from popcorn._snapshot import Snapshot
from popcorn._stop import Stop, stop_of
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine
    from popcorn._streams import UartPorts

_STOP_BITS = {0.5: 1, 1.0: 2, 1.5: 3, 2.0: 4}
_PARITY = {"none": 0, "even": 1, "odd": 2}
_WATCHDOG = 5.0
_TAP_KEEP = 1 << 20
_LOG = logging.getLogger("popcorn")
_UNDELIVERED = "a member stop the last run found is not delivered yet: run the system on first"


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
class SystemSnapshot:
    """A `LinkedSystem`'s whole state at a stop, `LinkedSystem.snapshot`, restorable into it
    any number of times with `LinkedSystem.restore`. In memory only.

    Attributes:
        members: Each member's own snapshot, keyed by name.
        state: The system's own state: the window, the pending barrier, each member's base and
            halt, and every link's frames in flight, armed edits and injections.
        target: The last TIME target, the base `run(for_=)` and `wait_for` count from, None
            before the first one.
    """

    members: Mapping[str, Snapshot]
    state: bytes
    target: VirtualTime | None


@dataclass(frozen=True, slots=True)
class TapFrame:
    """One frame on one direction of a link, as `UartTap` shows it. Times are system times.

    Attributes:
        data: Data bits, 0 for a break.
        index: Number of the frame among its sender's frames in this direction, from 0 at
            system start, or among the frames injected in this direction for an injected one:
            the index `UartTap.replace` and `drop` take for a sender's frame.
        start: Start-bit edge at the sender, at the receiver for an injected frame.
        end: Stop-bit end at the sender, the abort time for a lost frame, at the receiver for an
            injected frame.
        arrival: When the receiver sees the frame end: `end` plus the link's latency, `end` for
            an injected frame.
        towards: The receiving end, `"<member>.<UART>"`.
        injected: Whether `UartTap.inject` put it on the line.
        brk: Whether it is a break.
        lost: Whether the sender aborted it, its UART disabled mid-frame.
        dropped: Whether an armed `UartTap.drop` kept it off the line: the receiver saw an idle
            line.
    """

    data: int
    index: int
    start: VirtualTime
    end: VirtualTime
    arrival: VirtualTime
    towards: str
    injected: bool
    brk: bool
    lost: bool
    dropped: bool


@dataclass(frozen=True)
class _LinkWires:
    ends: tuple[str, str]
    wires: tuple[int, int]


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


@dataclass(eq=False)
class _Wait:
    until: int | None
    for_ps: int | None
    deadline_ps: int | None
    gate: _core.Predicate | None
    stop_when: Callable[[], bool] | None
    future: asyncio.Future[Stop]
    until_ps: int | None = None
    from_executed: int | None = None


@dataclass(eq=False)
class _Goal:
    future: _core.HostFuture
    task: asyncio.Task[object] | None
    releases: bool = False
    withdrawn: bool = False
    waits: tuple[_Wait, ...] = ()

    def done(self) -> bool:
        return self.future.done()


def _reached_waits(waits: Sequence[_Wait], stop: Stop, at: int, held: int) -> list[_Wait]:
    """The waits a run goal serving `waits` reached with `stop` at system time `at`, `held`
    the mask of waits whose predicate stopped it: none for a member stop."""
    if stop.reason is StopReason.TIME and stop.detail == "goal cap":
        return []
    reached: list[_Wait] = []
    for bit, wait in enumerate(waits):
        if stop.reason is StopReason.USER:
            own = bool(held >> bit & 1)
        elif stop.reason is StopReason.TIME:
            own = wait.until_ps is not None and wait.until_ps <= at
        elif stop.reason is StopReason.ADDRESS:
            own = wait.until == stop.pc
        else:
            own = False
        if own:
            reached.append(wait)
    return reached


def _held_of(raw: object) -> tuple[Stop, int, int]:
    info, at, held = cast(tuple[_core.StopInfo, int, int], raw)
    return stop_of(info), at, held


def _reached_of(raw: object) -> tuple[Stop, int]:
    info, at = cast(tuple[_core.StopInfo, int], raw)
    return stop_of(info), at


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

    Member goals: a member's `run`, `step`, `wait_for`, stream reads and `running()` become goals
    on the system's clock, one per member. Goals posted in one turn of the event loop start
    together, each at its member's position:

        a_stop, b_stop = await asyncio.gather(a.run(until="f_a"), b.run(until="f_b"))

    A member whose goal stops waits exactly at its stop, parked, and the system waits with it.
    The task that posted the goal holds it. `gather`, `TaskGroup`, `shield` and `ensure_future`
    post from a child task of their own, which releases the member as soon as it ends. The
    member is released, halted from its stop on, when its holder finishes, or awaits a popcorn
    future of another machine, or calls `member.halt()`. A new goal on it
    before that starts exactly at its stop, also after unrelated awaits such as
    `asyncio.sleep`. Members without a goal are halted: their cores execute nothing, their clocks,
    peripherals and DMA follow system time, their DBGMCU freeze bits apply. A task that keeps a
    member parked while it awaits something only a peer's goal can complete waits forever, and
    so does one that awaits its own member's future that needs time to pass, such as a write at
    a later time: nothing runs while the member is parked. After 5 s a warning names the member
    and the task, `member.halt()` releases it. Cancelling a goal's
    task withdraws the goal at the next pause, where the member then stands, a point that
    depends on wall timing. A goal posted while other goals run is admitted at the next pause,
    also wall-dependent. An error a member raises, from a handler or a model, stops the system:
    every pending goal fails with that error, noted with the member and its system time, and a
    goal stopped before it keeps its stop.

    Inside a member's own `running()`, its `wait_for`, stream reads and `elapse` wait as on a
    lone machine's free run, several at once: the run's goal stops for any of them, each
    resolves at its own stop, and the run goes on exactly from that stop for the others. A
    member stop, a breakpoint, FAULT or a handler's `stop()`, ends the run instead: every wait
    returns it, later waits return it, and leaving `running()` raises it. Where a wait starts
    depends on wall timing, as on a lone machine, except for waits called before the body's
    first await. Cancelling a wait lets the member run on. `Reached` waits for several addresses
    each stop at their own address, ADDRESS. A new wait whose member stands at its address leaves
    it first, as `run(until=)` does. Once the member executed an instruction after the wait
    began, standing at the address is its stop, also where the run's goal is re-posted for
    another wait. That depends on instruction counts only. `run` and `step` inside it raise
    MachineBusy, as on a lone machine's free run, and no budget applies.

    The system's own free run, `running()` or `start()` and `halt()`, runs every member's core
    as inside its own `running()`, all at once, until halted:

        async with system.running():
            stop = await controller.wait_for(controller.ref.var("done").eq(1))

    `wait_for` waits on the whole system instead, a predicate asked at fixed check points of
    system time, each a TIME stop: exact, and refused inside the free run.

    A member while the system exists: sync reads and writes raise MachineRunning while the system
    runs, and while the member has a pending goal, since its position at a pause depends on the
    quantum. They are allowed on a halted member while the system is stopped. `start`,
    `snapshot` and `restore` of a member raise MachineBusy, `halt` releases it, and `reset` goes
    through `LinkedSystem.reset`. `snapshot` and `restore` take and give back the whole
    system's state, every member included, while it is stopped. Its
    Python callbacks run on its own thread, concurrently with other members' callbacks: Python
    state they share sees an order that depends on thread timing, so keep callback state per
    member and couple members through links. The system owns the members' UARTs named in links:
    frames each transmits still reach its own `port`, whose `write` raises RuntimeError until
    `close()`. `tap` shows the frames on a link and injects, replaces or drops frames.

    The system holds its members until `close()`, also run by `with`.
    """

    __slots__ = (
        "__weakref__",
        "_admitting",
        "_closed",
        "_ends",
        "_failed",
        "_flight",
        "_free",
        "_free_error",
        "_goal_cap",
        "_goals",
        "_halt_task",
        "_halting",
        "_handle",
        "_index",
        "_kicking",
        "_links",
        "_loop",
        "_machines",
        "_names",
        "_native",
        "_opens",
        "_origin",
        "_pausing",
        "_rebuilds",
        "_run_ends",
        "_system_run",
        "_taps",
        "_target",
        "_tokens",
        "_undecided",
        "_unlink",
        "_waits",
        "_warned",
        "_withdrawals",
    )

    def __init__(
        self,
        members: Mapping[str, Machine],
        links: Sequence[UartLink],
        *,
        offsets: Mapping[str, VirtualDuration] | None = None,
        on_member_reset: Literal["reset", "stop"] = "reset",
        goal_cap: VirtualDuration | None = None,
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
            goal_cap: Virtual time a member goal may run from its start, None for no cap: a goal
                whose member sleeps while its peers keep scheduling events stops with TIME,
                detail `goal cap`, once its start plus the cap is reached.
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
            ValueError: `slice_instructions`, `slice_wall_budget` or `goal_cap` not above 0.
            TypeError: `goal_cap` is no VirtualDuration.
        """
        if slice_instructions <= 0 or not slice_wall_budget > 0:
            raise ValueError("slice_instructions and slice_wall_budget must be above 0")
        if goal_cap is not None:
            if not isinstance(cast(object, goal_cap), VirtualDuration):
                raise TypeError(
                    f"goal_cap must be a VirtualDuration, not {type(goal_cap).__name__}"
                )
            if goal_cap.ps <= 0:
                raise ValueError("goal_cap must be above 0")
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
        self._links: list[_LinkWires] = []
        self._taps: dict[int, UartTap] = {}
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
        self._unlink = weakref.finalize(self, _unmark, self._mark_linked())
        self._origin = max(offset_ps)
        self._closed = False
        self._failed: BaseException | None = None
        self._flight: asyncio.Future[_Pause] | None = None
        self._native: _core.HostFuture | None = None
        self._free = False
        self._halting = False
        self._halt_task: asyncio.Future[None] | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._ends: list[tuple[int, Stop]] = []
        self._free_error: BaseException | None = None
        self._pausing = False
        self._target: int | None = None
        self._goal_cap = None if goal_cap is None else goal_cap.ps
        self._goals: dict[int, _Goal] = {}
        self._opens: dict[int, _Goal] = {}
        self._waits: dict[int, list[_Wait]] = {}
        self._rebuilds: set[int] = set()
        self._run_ends: dict[int, Stop] = {}
        self._undecided: dict[int, asyncio.Task[object] | None] = {}
        self._withdrawals: set[int] = set()
        self._warned: set[tuple[int, int]] = set()
        self._system_run = False
        self._kicking = False
        self._admitting = 0
        self._tokens = [
            self._machines[name]._handle.log_token()  # pyright: ignore[reportPrivateUsage]
            for name in self._names
        ]
        for machine in self._machines.values():
            machine._system = cast(Any, weakref.ref(self))  # pyright: ignore[reportPrivateUsage]
        watch_awaits(self)

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
        wires: list[int] = []
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
            wires.append(self._handle.add_wire(spec, sender_link, receiver_link))
        self._links.append(
            _LinkWires(
                (
                    f"{self._names[ends[0][0]]}.{ends[0][2]}",
                    f"{self._names[ends[1][0]]}.{ends[1][2]}",
                ),
                (wires[0], wires[1]),
            )
        )

    def _mark_linked(self) -> list[tuple[UartPorts, str]]:
        """Mark each linked UART on its member's ports, so its stream and pty refuse, and
        return the marks for `_unmark`."""
        marked: list[tuple[UartPorts, str]] = []
        for wired in self._links:
            for end in wired.ends:
                name, _, unit = end.partition(".")
                ports = self._machines[name]._ports  # pyright: ignore[reportPrivateUsage]
                ports.linked.add(unit)
                marked.append((ports, unit))
        return marked

    def _end(
        self, index: int, side: str, end: str, used: set[tuple[str, str]]
    ) -> tuple[int, _core.UartQueue, str]:
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

    def to_local(self, member: str | Machine, time: VirtualTime) -> VirtualTime:
        """The local time of `member`, its `Machine.time` frame, at system time `time`: what a
        member's `at=` takes, which counts in its own local time.

            await c.write((c.ref.mem.u32[x], 5), at=system.to_local("c", t))

        The member's base, its system time minus its local time, is fixed at construction and
        moved by a reset of the member. Allowed while the system runs, from any thread, a
        member's callbacks included, such as an intercept converting its own `Machine.time`.
        A reset under `on_member_reset="reset"` moves the base mid-run, so a call during it
        may use the base before or after the reset.

        Raises:
            KeyError: `member` is no member of this system.
            TypeError: `time` is no VirtualTime.
            ValueError: `time` lies before the member's local time 0, its last reset.
            EmulationError: The system was closed.
        """
        base = self._base_of(member, time)
        local = time.ps - base
        if local < 0:
            raise ValueError(f"{time} is before the last reset of {self._name_of(member)}")
        return VirtualTime(local)

    def to_system(self, member: str | Machine, time: VirtualTime) -> VirtualTime:
        """The system time at local time `time` of `member`: the inverse of `to_local`, for a
        member's `Machine.time`, `Stop.time`, `Readout.time` or a stream frame's `at`.

        Raises:
            KeyError: `member` is no member of this system.
            TypeError: `time` is no VirtualTime.
            ValueError: The result lies before system time 0.
            EmulationError: The system was closed.
        """
        base = self._base_of(member, time)
        system_ps = time.ps + base
        if system_ps < 0:
            raise ValueError(
                f"local time {time} of {self._name_of(member)} is before system time 0"
            )
        return VirtualTime(system_ps)

    def _name_of(self, member: str | Machine) -> str:
        name = member if isinstance(member, str) else member.name
        if name not in self._index or (
            not isinstance(member, str) and self._machines[name] is not member
        ):
            raise KeyError(f"{member!r} is no member of {self!r}")
        return name

    def _base_of(self, member: str | Machine, time: VirtualTime) -> int:
        index = self._index[self._name_of(member)]
        if not isinstance(cast(object, time), VirtualTime):
            raise TypeError(f"time must be a VirtualTime, not {type(time).__name__}")
        if self._closed:
            raise EmulationError("the linked system was closed")
        return self._handle.base_of_ps(index)

    def time_of(self, member: str) -> VirtualTime:
        """System time of `member`, its local time plus its base.

        Raises:
            KeyError: No such member.
            MachineRunning: The system runs.
        """
        index = self._index[member]
        self._require_stopped()
        return VirtualTime(self._handle.time_of_ps(index))

    def tap(self, end: str) -> UartTap:
        """The tap of the link with end `end`, `"<member>.<UART>"`, either end or an alias of
        its UART: the same `UartTap` for both ends. It stays readable after `close()`.

        Raises:
            ValueError: No link has the end `end`.
        """
        named = self._end_name(end)
        for index, wired in enumerate(self._links):
            if named in wired.ends:
                tap = self._taps.get(index)
                if tap is None:
                    tap = self._taps[index] = UartTap(self, wired)
                return tap
        raise ValueError(f"{end!r} is no end of a link of {self!r}")

    def _end_name(self, end: object) -> str | None:
        if not isinstance(end, str):
            return None
        name, _, unit = end.partition(".")
        machine = self._machines.get(name)
        if machine is None or not unit:
            return None
        return f"{name}.{machine._ports.serving(unit)}"  # pyright: ignore[reportPrivateUsage]

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
        self._refuse_run()
        target = self._target_of(for_, until)
        self._system_run = True
        try:
            return await self._run_to(target)
        finally:
            self._system_run = False

    async def _run_to(self, target: int) -> SystemStop:
        self._release_undecided()
        return self._system_stop(await self._resume(target), target)

    async def wait_for(
        self,
        predicate: Callable[[LinkedSystem], bool],
        *,
        every: VirtualDuration,
        for_: VirtualDuration | None = None,
    ) -> SystemStop:
        """Run every member's core until `predicate(system)` holds at a check point.

        The check points are system times `base + k * every`, k from 1, where `base` is the
        target of the previous TIME stop, else the largest offset, as for `run(for_=)`. At each
        one the system makes a TIME stop, every member parked at it, and `predicate` runs on
        the caller's thread with sync access to every member. The stop is the first check point
        where it holds, whatever the quantum, the slicing or the wall timing: the predicate is
        never asked between check points. Each check point is a TIME target, so a later
        `run(for_=)` or `wait_for` counts from the stop.

        With `for_`, the wait ends at `base + for_` with a TIME stop if `predicate` held at no
        check point before it, and is not asked there. A member stop or an IDLE pause on the way
        ends the wait as it ends `run`.

        Raises:
            TypeError: `every` or `for_` is no VirtualDuration.
            ValueError: `every` not above 0, or `for_` negative.
            MachineBusy: The system runs, runs freely, its `running()` included, or a member has
                a goal pending.
            EmulationError: The system was closed, or stopped on an error before.
            BaseException: What `predicate` raised, or the error that stopped the system.

        Returns:
            USER at the check point where `predicate` held, each member's stop a TIME stop
            there. TIME at `base + for_`. A member stop or IDLE as `run` returns them.
        """
        _check_every(every, for_)
        self._require_usable()
        await self._settled()
        self._refuse_run()
        base = self._origin if self._target is None else self._target
        deadline = None if for_ is None else base + for_.ps
        self._system_run = True
        try:
            point = base
            while True:
                point += every.ps
                if deadline is not None and point >= deadline:
                    return await self._run_to(deadline)
                stop = await self._run_to(point)
                if stop.reason is not StopReason.TIME or stop.member is not None:
                    return stop
                if predicate(self):
                    return replace(stop, reason=StopReason.USER)
        finally:
            self._system_run = False

    def start(self) -> None:
        """Run every member's core until `halt()`, a free run of the system.

        Every member runs as inside its own `running()`, all started together at their
        positions, on the system's clock. A member stop, FAULT, a breakpoint or a reset under
        `on_member_reset="stop"`, ends that member's run: it is halted from its stop on, the
        others go on, and `halt()` raises it. When every member sleeps with nothing due the
        system sleeps until a command, a stamp, an injection or a pty byte wakes a member.
        Inside, a member's `wait_for`, stream reads and `elapse` wait on the free run as inside
        its own `running()`, and return the stop that ended its run once it ended. A member's
        own `running()`, `system.run` and `system.start` raise MachineBusy.

        The members start at the event loop's next turn: a member wait called before the
        caller's next await starts exactly at the free run's start, a later one at a point
        that depends on wall timing. Call it from a coroutine: the system follows its pauses on
        the running event loop.

        Raises:
            MachineBusy: The system runs, runs freely, or a member has a goal pending.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: No running event loop.
        """
        self._require_usable()
        loop = asyncio.get_running_loop()
        self._refuse_run()
        if self._opens:
            name = self._names[min(self._opens)]
            raise MachineBusy(f"{name} runs in its own running(): leave it first")
        self._release_undecided()
        try:
            for index in range(len(self._names)):
                goal = _Goal(
                    self._handle.post_goal(
                        index, None, None, None, None, None, None, True, self._goal_cap
                    ),
                    None,
                    releases=True,
                )
                self._goals[index] = goal
                self._opens[index] = goal
        except BaseException:
            for index in sorted(self._opens):
                self._handle.withdraw(index)
            self._goals.clear()
            self._opens.clear()
            raise
        self._free = True
        self._loop = loop
        self._ends = []
        self._free_error = None
        self._schedule_kick()

    def halt(self) -> None:
        """End the free run `start()` started. The system pauses at its next barrier, every
        member is halted where it stands, and this returns once every member parked, waiting
        with the GIL released. Where the members stand depends on wall timing and the quantum,
        as a lone machine's `halt()` lands at a wall-dependent boundary, and so do the member
        stops it reports: a stop at or before that pause is raised, a later one never happens. A
        wait of a member still pending raises EmulationError. A no-op when no free run is
        active.

        From a thread other than the one running the event loop that started the free run, the
        halt runs on that loop, `call_soon_threadsafe`, and this waits for it with the GIL
        released: a loop blocked on this thread deadlocks, and so does a loop stopped and never
        run or closed again. When that loop or the system closes before the halt began there,
        the halt runs here. On an open system a callback of a machine is refused first, free
        run or not, so a refused halt keeps what it would raise for the next one.

        Raises:
            EmulationError: A member's run ended on a stop during the free run: FaultError for
                FAULT, BudgetExceeded for BUDGET, EmulationError otherwise, carrying the first
                such stop by system time, the others named in its notes. Raised once.
            BaseException: The error that stopped the system during the free run, raised once.
            RuntimeError: Called from a callback of a machine, the system open.
            MachineRunning: Called inside a parked section of a machine, the system open.
        """
        if not self._closed:
            self._handle.check_caller()
        loop = self._loop
        if (
            self._free
            and loop is not None
            and not _on_loop(loop)
            and not loop.is_closed()
            and self._halt_from(loop)
        ):
            return
        if self._free:
            halting = self._halting
            self._halting = True
            try:
                native = self._native
                if native is not None:
                    self._pausing = True
                    self._handle.request_pause()
                    pause = _Pause.of(native.result(None))
                    if self._took(native):
                        self._paused(pause, None)
                self._end_free()
            finally:
                self._halting = halting
        self._raise_ends()

    def _halt_from(self, loop: asyncio.AbstractEventLoop) -> bool:
        """Run `halt()` on `loop` and wait for it with the GIL released: True once it ran
        there, False when `loop` or the system closed before it began, the halt then the
        caller's."""
        done: concurrent.futures.Future[None] = concurrent.futures.Future()

        def forwarded() -> None:
            if not done.set_running_or_notify_cancel():
                return
            try:
                self.halt()
            except BaseException as error:
                done.set_exception(error)
            else:
                done.set_result(None)

        try:
            loop.call_soon_threadsafe(forwarded)
        except RuntimeError:
            return False
        while not concurrent.futures.wait([done], timeout=0.05).done:
            if (loop.is_closed() or self._closed) and done.cancel():
                return False
        done.result()
        return True

    @contextlib.asynccontextmanager
    async def running(self) -> AsyncGenerator[None]:
        """Run the system freely while the body awaits other things: `start()` on entry,
        `halt()` on exit, without blocking the event loop.

        Leaving the body raises what `halt()` raises. When the body itself raises, a
        cancellation included, the body's exception wins, with a note naming what leaving the
        free run raised.

        Raises:
            MachineBusy: As `start()`.
            EmulationError: A member's run ended on a stop, as `halt()`.
            BaseException: The error that stopped the system during the free run.
        """
        self.start()
        try:
            yield
        except BaseException as error:
            try:
                await self._halt_free()
            except Exception as failed:
                error.add_note(f"leaving the system's free run raised: {failed!r}")
            raise
        await self._halt_free()

    async def _halt_free(self) -> None:
        if self._free:
            task = self._halt_task
            if task is None:
                self._halting = True
                task = asyncio.ensure_future(self._halting_free())
                if not task.done():
                    self._halt_task = task
            await asyncio.shield(task)
        self._raise_ends()

    async def _halting_free(self) -> None:
        try:
            while (flight := self._flight) is not None:
                self._pausing = True
                self._handle.request_pause()
                with contextlib.suppress(Exception):
                    await asyncio.shield(flight)
            self._end_free()
        finally:
            self._halting = False
            self._halt_task = None

    def _end_free(self) -> None:
        """Withdraw the free run's open goals and release the members its waiters left parked."""
        self._free = False
        opens, self._opens = self._opens, {}
        self._run_ends.clear()
        self._rebuilds.clear()
        for index in sorted(self._waits):
            self._fail_waits(index)
        for index, goal in sorted(opens.items()):
            if self._goals.get(index) is goal:
                del self._goals[index]
                goal.withdrawn = True
                if not goal.done() and not self._closed and self._failed is None:
                    self._handle.withdraw(index)
        if self._closed or self._failed is not None:
            return
        for index in self._handle.undecided():
            if index not in self._undecided:
                self._handle.release(index)

    def _raise_ends(self) -> None:
        error, self._free_error = self._free_error, None
        ends, self._ends = self._ends, []
        if error is not None:
            raise error
        if not ends:
            return
        from popcorn import _machine

        index, stop = ends[0]
        name = self._names[index]
        prefix = _machine.error_prefix(self._machines[name])
        raised = _machine._stop_error(  # pyright: ignore[reportPrivateUsage]
            stop, "its run in the system's free run", prefix
        )
        for other, more in ends[1:]:
            raised.add_note(f"{self._names[other]} also stopped: {more}")
        raise raised

    def reset(self, member: str, kind: ResetKind = ResetKind.POWER_ON) -> None:
        """Reset member `member` while the system is stopped, or paused at a member goal's
        stop, as `Machine.reset` resets a lone machine: memory is kept. Its frame in flight is
        lost, frames reaching it within a link's latency after the reset are dropped, and its
        base moves to the reset's system time, so its system time runs on unchanged while its
        local time restarts at 0. It is no longer held at a stop: a member stop or its goal's
        stop that left it there, or a stop of it the last run found and did not deliver yet,
        is dropped, and it runs its core on the next `run`, as a lone machine runs on after
        its reset. Without a goal it stays halted while other members' goals run.
        `member.reset()` does the same.

        Raises:
            KeyError: No such member.
            TypeError: `kind` is no ResetKind.
            MachineRunning: The system runs, runs freely, or the member has a goal pending.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine.
        """
        index = self._index[member]
        if not isinstance(cast(object, kind), ResetKind):
            raise TypeError(f"kind must be a ResetKind, not {type(kind).__name__}")
        self._require_usable()
        self._require_halted("reset")
        if index in self._goals:
            raise MachineRunning(f"{member} has a goal pending: no reset before it ends")
        self._handle.reset(index, _core.ResetKind(kind.value))
        self._undecided.pop(index, None)
        self._machines[member]._last_stop = None  # pyright: ignore[reportPrivateUsage]

    def snapshot(self) -> SystemSnapshot:
        """The whole system's state while it is stopped: every member's snapshot and the
        system's own state, links included, frames in flight too.

        A member still held at its goal's stop is released first, halted from that stop on,
        as the next `run` does: a goal posted on it then starts at the same point.

        Raises:
            MachineRunning: The system runs, runs freely, a member has a goal pending or runs in
                its own `running()`, or a member stop the last run found is not delivered yet:
                `run` delivers it.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine.
        """
        self._require_quiet("snapshot")
        self._handle.check_caller()
        self._release_undecided()
        try:
            state = self._handle.save()
        except RuntimeError as error:
            raise MachineRunning(_UNDELIVERED) from error
        members = {
            name: machine._snapshot()  # pyright: ignore[reportPrivateUsage]
            for name, machine in self._machines.items()
        }
        target = None if self._target is None else VirtualTime(self._target)
        return SystemSnapshot(members, state, target)

    def restore(self, snapshot: SystemSnapshot) -> None:
        """Restore a snapshot this system took, while it is stopped: the members, their bases
        and halts, the links with their frames in flight, armed edits and injections, and the
        last TIME target. A run after it equals the run that followed the snapshot. Frames
        every tap holds and members' UART frames not read yet are dropped, as
        `Machine.restore` drops them, and `UartTap.dropped` restarts at 0, as a lone
        `port.dropped` does. `snapshot.state` is checked before any member is touched. When
        restoring a member or the state still fails, every member gets its state from before
        the call back, and the first error doing so is raised from the original one. After an
        error stopped the system it clears the error, as a lone machine restores after an
        error, and the system runs again.

        Raises:
            TypeError: `snapshot` is no SystemSnapshot.
            ValueError: `snapshot` holds other members.
            ConfigError: A member's snapshot was taken by another machine, or `snapshot.state`
                is not a state this system's members and links load. Nothing was touched.
            MachineRunning: As `snapshot`, or inside `wait_for`'s predicate.
            EmulationError: The system was closed.
            RuntimeError: Called from a callback of a machine.
        """
        if not isinstance(cast(object, snapshot), SystemSnapshot):
            raise TypeError(f"snapshot must be a SystemSnapshot, not {type(snapshot).__name__}")
        failed = self._failed is not None
        self._require_quiet("restore", failed_ok=True)
        if self._system_run:
            raise MachineRunning("the linked system runs a wait_for: no restore inside it")
        if sorted(snapshot.members) != sorted(self._names):
            raise ValueError(
                f"the snapshot holds members {sorted(snapshot.members)}, not {sorted(self._names)}"
            )
        for name, machine in self._machines.items():
            machine._check_snapshot(snapshot.members[name])  # pyright: ignore[reportPrivateUsage]
        self._handle.check_caller()
        if not failed:
            self._release_undecided()
            try:
                self._handle.save()
            except RuntimeError as error:
                raise MachineRunning(_UNDELIVERED) from error
        self._handle.check_state(snapshot.state)
        kept = {
            name: machine._snapshot()  # pyright: ignore[reportPrivateUsage]
            for name, machine in self._machines.items()
        }
        try:
            for name, machine in self._machines.items():
                machine._restore(snapshot.members[name])  # pyright: ignore[reportPrivateUsage]
            self._handle.load(snapshot.state)
        except BaseException as error:
            unrestored: BaseException | None = None
            for name, machine in self._machines.items():
                try:
                    machine._restore(kept[name])  # pyright: ignore[reportPrivateUsage]
                except BaseException as failed:
                    unrestored = unrestored or failed
            if unrestored is not None:
                raise unrestored from error
            raise
        self._failed = None
        self._undecided.clear()
        self._target = None if snapshot.target is None else snapshot.target.ps
        for index, wired in enumerate(self._links):
            tap = self._taps.get(index)
            if tap is not None:
                tap._discard()  # pyright: ignore[reportPrivateUsage]
            else:
                for wire in wired.wires:
                    self._handle.tap_take(wire)

    def _require_halted(self, what: str) -> None:
        if self._flight is not None or self._free or self._halting:
            raise MachineRunning(f"the linked system runs: no {what} while it runs")

    def _require_quiet(self, what: str, *, failed_ok: bool = False) -> None:
        if failed_ok and self._closed:
            raise EmulationError("the linked system was closed")
        if not failed_ok:
            self._require_usable()
        self._require_halted(what)
        if self._opens:
            name = self._names[min(self._opens)]
            raise MachineRunning(f"{name} runs in its own running(): no {what} inside it")
        if self._goals:
            name = self._names[min(self._goals)]
            raise MachineRunning(f"a goal of {name} is pending: no {what} before it ends")

    def _release_undecided(self) -> None:
        for index in sorted(self._undecided):
            self._handle.release(index)
        self._undecided.clear()

    def _refuse_run(self) -> None:
        if self._free or self._halting:
            raise MachineBusy("the linked system runs freely: halt() it first")
        if self._flight is not None or self._system_run:
            raise MachineBusy("the linked system runs")
        if self._goals:
            name = self._names[min(self._goals)]
            raise MachineBusy(f"a goal of {name} is pending: await it, or cancel its task")

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

    def _launch(self, until_ps: int | None) -> asyncio.Future[_Pause]:
        future = self._handle.resume(until_ps)
        self._native = future
        flight = asyncio.ensure_future(self._flown(future, until_ps))
        if not flight.done():
            self._flight = flight
        return flight

    async def _resume(self, until_ps: int | None) -> _Pause:
        flight = self._launch(until_ps)
        try:
            return await asyncio.shield(flight)
        except asyncio.CancelledError:
            if not flight.done():
                self._pausing = True
                self._handle.request_pause()
            raise

    async def _flown(self, future: _core.HostFuture, until_ps: int | None) -> _Pause:
        taken = False
        try:
            try:
                pause = await awaited(future, _Pause.of, cancel=False)
            except asyncio.CancelledError:
                with contextlib.suppress(EmulationError):
                    self._handle.request_pause()
                pause = _Pause.of(future.result(None))
        finally:
            taken = self._took(future)
        if taken:
            self._paused(pause, until_ps)
        return pause

    def _took(self, future: _core.HostFuture) -> bool:
        """End the flight of resume `future`, False when `halt()` already did."""
        if self._native is not future:
            return False
        self._flight = None
        self._native = None
        self._pausing = False
        return True

    def _paused(self, pause: _Pause, until_ps: int | None) -> None:
        if pause.reason is _core.PauseReason.system_time and until_ps is not None:
            self._target = until_ps
        elif pause.reason is _core.PauseReason.delivered and pause.member is not None:
            ended = self._goals.get(pause.member)
            if ended is not None and self._opens.get(pause.member) is ended:
                self._run_stopped(pause.member, ended)
            elif ended is not None and (ended.releases or ended.withdrawn):
                self._handle.release(pause.member)
            elif ended is None:
                self._undecided.setdefault(pause.member, None)
        elif pause.reason is _core.PauseReason.error:
            self._failed = pause.error
            if pause.error is not None and pause.member is not None:
                note = (
                    f"raised by member {self._names[pause.member]} at system time "
                    f"{VirtualTime(pause.at)}"
                )
                pause.error.add_note(note)
            if self._free:
                self._free_error = pause.error
        self._goals = {index: goal for index, goal in self._goals.items() if not goal.done()}
        if self._failed is None and not self._closed:
            for index in sorted(self._withdrawals):
                if index in self._goals:
                    self._handle.withdraw(index)
                    del self._goals[index]
            for index in sorted(self._rebuilds):
                self._repost(index)
        else:
            for index in sorted(self._waits):
                self._fail_waits(index)
        self._withdrawals.clear()
        self._rebuilds.clear()
        self._schedule_kick()

    def _run_stopped(self, index: int, goal: _Goal) -> None:
        """The goal of member `index`'s run stopped, at a delivery. Each wait whose own bounds
        or predicate it reached resolves with the stop, and the run goes on from there exactly
        for the others. Any other stop is a member stop: it ends the run, every wait resolves
        with it, and the member is released at it."""
        raw = goal.future.result(0.0)
        stop, at, held = _held_of(raw) if goal.waits else (*_reached_of(raw), 0)
        reached = _reached_waits(goal.waits, stop, at, held)
        waits = self._waits.get(index, [])
        if not reached:
            self._run_ends[index] = stop
            reached = waits
            waits = []
            self._handle.release(index)
            if self._free:
                self._ends.append((index, stop))
        for wait in reached:
            if not wait.future.done():
                wait.future.set_result(stop)
        self._waits[index] = [wait for wait in waits if wait not in reached]
        if index not in self._run_ends:
            self._rebuilds.add(index)
            goal.withdrawn = True

    def _fail_waits(self, index: int) -> None:
        name = self._names[index]
        for wait in self._waits.pop(index, []):
            if not wait.future.done():
                wait.future.set_exception(EmulationError(f"{name}: the run this wait was in ended"))

    def _repost(self, index: int) -> None:
        """Give member `index`'s run a goal serving its current waits, or a plain open goal for
        none, from where it stands: withdraw a goal still running, re-issue at a delivered stop."""
        current = self._opens.get(index)
        if current is None or index in self._run_ends:
            return
        if self._goals.get(index) is current:
            del self._goals[index]
            if not current.done():
                self._handle.withdraw(index)
        waits = tuple(self._waits.get(index, ()))
        executed = self._handle.executed_of(index) if waits else 0
        for wait in waits:
            if wait.from_executed is None:
                wait.from_executed = executed
            if wait.until_ps is None and wait.for_ps is not None:
                wait.until_ps = self._handle.time_of_ps(index) + wait.for_ps
            elif wait.until_ps is None and wait.deadline_ps is not None:
                machine = self._machines[self._names[index]]
                base = self._handle.time_of_ps(index) - machine.time.ps
                wait.until_ps = base + wait.deadline_ps
        if waits:
            untils = list(dict.fromkeys(w.until for w in waits if w.until is not None))
            future = self._handle.post_waits(
                index,
                untils,
                [
                    until
                    for until in untils
                    if any(w.until == until and w.from_executed != executed for w in waits)
                ],
                min((w.until_ps for w in waits if w.until_ps is not None), default=None),
                [wait.gate for wait in waits],
                [wait.stop_when for wait in waits],
                self._goal_cap,
            )
            goal = _Goal(future, None, waits=waits)
        else:
            future = self._handle.post_goal(
                index, None, None, None, None, None, None, True, self._goal_cap
            )
            goal = _Goal(future, None, releases=True)
        self._undecided.pop(index, None)
        self._opens[index] = goal
        self._goals[index] = goal

    def _rebuild(self, index: int) -> None:
        """Re-post member `index`'s run goal for its current waits: at once while paused, else
        at the pause requested now."""
        if self._flight is None:
            self._repost(index)
            self._schedule_kick()
            return
        current = self._opens.get(index)
        if current is not None:
            current.withdrawn = True
        self._rebuilds.add(index)
        self._pausing = True
        self._handle.request_pause()

    def _schedule_kick(self) -> None:
        """Kick at the loop's next turn, once: goals posted in the same turn start together."""
        if self._kicking:
            return
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return
        self._kicking = True
        loop.call_soon(self._kicked)

    def _kicked(self) -> None:
        self._kicking = False
        self._kick()

    def _kick(self) -> None:
        """Resume the system for the pending goals once every delivered member is decided and
        no goal waits to be admitted."""
        if self._closed or self._failed is not None or self._flight is not None:
            return
        if self._admitting or self._halting:
            return
        if not self._goals or self._handle.undecided():
            return
        self._launch(None)

    async def _goal(
        self,
        machine: Machine,
        *,
        until: int | None = None,
        budget: int | None = None,
        exact: int | None = None,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
        stop_when: Callable[[], bool] | None = None,
        gate: _core.Predicate | None = None,
        run: bool = False,
    ) -> Stop:
        """Run a goal of member `machine`, parked at its stop once delivered: see the class.
        Inside the member's own `running()` it joins the run instead, `_join`, except a `run`,
        refused there as on a lone machine's free run."""
        index = self._index[machine.name]
        if index in self._opens:
            if run:
                raise MachineBusy(f"{machine.name} runs in its running(): no run inside it")
            return await self._join(
                index,
                machine,
                until=until,
                exact=exact,
                for_=for_,
                deadline=deadline,
                stop_when=stop_when,
                gate=gate,
            )
        return await self._reached(
            index,
            await self._post(
                index,
                machine,
                until=until,
                budget=budget,
                exact=exact,
                for_=for_,
                deadline=deadline,
                stop_when=stop_when,
                gate=gate,
                open_=False,
            ),
        )

    async def _post(
        self,
        index: int,
        machine: Machine,
        *,
        until: int | None,
        budget: int | None,
        exact: int | None,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
        stop_when: Callable[[], bool] | None,
        gate: _core.Predicate | None,
        open_: bool,
        releases: bool = False,
    ) -> _Goal:
        self._admitting += 1
        try:
            return await self._admit(
                index,
                machine,
                until=until,
                budget=budget,
                exact=exact,
                for_=for_,
                deadline=deadline,
                stop_when=stop_when,
                gate=gate,
                open_=open_,
                releases=releases,
            )
        finally:
            self._admitting -= 1
            self._schedule_kick()

    async def _admit(
        self,
        index: int,
        machine: Machine,
        *,
        until: int | None,
        budget: int | None,
        exact: int | None,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
        stop_when: Callable[[], bool] | None,
        gate: _core.Predicate | None,
        open_: bool,
        releases: bool,
    ) -> _Goal:
        self._require_usable()
        if index not in self._withdrawals:
            self._refuse_goal(index)
        while (flight := self._flight) is not None:
            self._pausing = True
            self._handle.request_pause()
            with contextlib.suppress(Exception):
                await asyncio.shield(flight)
            self._require_usable()
            self._refuse_goal(index)
        task = asyncio.current_task()
        until_ps: int | None = None
        if for_ is not None:
            until_ps = self._handle.time_of_ps(index) + for_.ps
        elif deadline is not None:
            base = self._handle.time_of_ps(index) - machine.time.ps
            until_ps = base + deadline.ps
        future = self._handle.post_goal(
            index, until, budget, exact, until_ps, stop_when, gate, open_, self._goal_cap
        )
        self._undecided.pop(index, None)
        goal = _Goal(future, task, releases)
        self._goals[index] = goal
        self._schedule_kick()
        return goal

    async def _open_goal(self, machine: Machine) -> None:
        """Start an open goal on member `machine`, for its `running()`, kept by the system so no
        frame of the body holds its future."""
        if self._free:
            raise MachineBusy(
                f"the linked system runs freely: {machine.name} runs in the system's running()"
            )
        index = self._index[machine.name]
        self._opens[index] = await self._post(
            index,
            machine,
            until=None,
            budget=None,
            exact=None,
            for_=None,
            deadline=None,
            stop_when=None,
            gate=None,
            open_=True,
            releases=True,
        )

    async def _join(
        self,
        index: int,
        machine: Machine,
        *,
        until: int | None,
        exact: int | None,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
        stop_when: Callable[[], bool] | None,
        gate: _core.Predicate | None,
    ) -> Stop:
        """A wait of a member inside its own run, `running()`: it joins the run's goal, which
        stops for any of its waits, each resolving at its own stop as a lone machine's free run
        waiters do, while the run goes on exactly from each stop for the others. A member stop
        ends the run and every wait with it, and later waits return it. Where a wait starts
        depends on wall timing, as on a lone machine, unless the run's goal has not started yet.
        Budgets do not apply inside a run, as on a lone machine's free run."""
        self._require_usable()
        ended = self._run_ends.get(index)
        if ended is not None:
            return ended
        if self._halting:
            raise EmulationError(f"{machine.name}: the run this wait was in ended")
        if exact is not None:
            raise MachineBusy(f"{machine.name} runs in its running(): no step inside it")
        waits = self._waits.setdefault(index, [])
        wait = _Wait(
            until,
            None if for_ is None else for_.ps,
            None if deadline is None else deadline.ps,
            gate,
            stop_when,
            asyncio.get_running_loop().create_future(),
        )
        waits.append(wait)
        self._rebuild(index)
        future = wait.future
        del wait
        try:
            return await future
        except asyncio.CancelledError:
            if not future.done() or future.cancelled():
                kept = self._waits.get(index)
                if kept is not None and self._opens.get(index) is not None:
                    self._waits[index] = [w for w in kept if w.future is not future]
                    self._rebuild(index)
            raise
        finally:
            del future

    async def _close_open(self, machine: Machine) -> Stop | None:
        """End the run of member `machine`: its pending waits raise EmulationError, its goal is
        withdrawn at the next pause, and the member stop that ended the run, if one did, is
        returned, the member released at it."""
        index = self._index[machine.name]
        goal = self._opens.pop(index)
        self._fail_waits(index)
        ended = self._run_ends.pop(index, None)
        if ended is not None:
            return ended
        if not goal.done():
            self._withdraw(index, goal)
        future, waits = goal.future, goal.waits
        if self._goals.get(index) is goal and goal.done():
            del self._goals[index]
        del goal
        try:
            raw = await awaited(future, lambda raw: raw, cancel=False)
        except Exception:
            del future
            await self._landed()
            if self._failed is not None or self._closed:
                raise
            return None
        del future
        await self._landed()
        stop, at, held = _held_of(raw) if waits else (*_reached_of(raw), 0)
        return None if _reached_waits(waits, stop, at, held) else stop

    def _refuse_goal(self, index: int) -> None:
        if self._system_run:
            raise MachineBusy("the linked system runs a system run: await it first")
        if self._halting:
            raise MachineBusy("the linked system's free run is halting")
        if index in self._goals:
            raise MachineBusy(f"{self._names[index]} has a goal pending: one goal per member")

    async def _reached(self, index: int, goal: _Goal) -> Stop:
        future, task = goal.future, goal.task
        try:
            stop, _ = await awaited(future, _reached_of, cancel=False)
            await self._landed()
        except asyncio.CancelledError:
            self._withdraw(index, goal)
            del goal, future
            raise
        except BaseException:
            del goal, future
            await self._landed()
            raise
        del future
        if self._goals.get(index) is goal:
            del self._goals[index]
        del goal
        self._undecided[index] = task
        if task is not None:
            task.add_done_callback(lambda _: self._release(index, task))
            asyncio.get_running_loop().call_later(_WATCHDOG, self._watchdog, index, task, stop)
        return stop

    async def _landed(self) -> None:
        """Wait for the flight whose pause delivered a goal, so its bookkeeping is done."""
        flight = self._flight
        if flight is not None:
            with contextlib.suppress(Exception):
                await asyncio.shield(flight)

    def _withdraw(self, index: int, goal: _Goal) -> None:
        """Withdraw `goal` of member `index`, its task cancelled. A goal already delivered is
        released instead, at once while paused, else by the pause that delivers it."""
        goal.withdrawn = True
        if self._closed or self._failed is not None:
            return
        if self._flight is not None:
            if self._goals.get(index) is goal and not goal.done():
                self._withdrawals.add(index)
                self._pausing = True
                self._handle.request_pause()
            return
        if self._goals.get(index) is goal:
            del self._goals[index]
            if not goal.done():
                self._handle.withdraw(index)
        if goal.done() and index not in self._undecided and index in self._handle.undecided():
            self._handle.release(index)
        self._schedule_kick()

    def _release(self, index: int, owner: asyncio.Task[object] | None) -> None:
        """Release member `index`, parked at its goal's stop, when `owner` still holds it, None
        for any holder."""
        if index not in self._undecided:
            return
        if owner is not None and self._undecided[index] is not owner:
            return
        del self._undecided[index]
        if self._closed or self._failed is not None:
            return
        if index in self._handle.undecided():
            self._handle.release(index)
        self._schedule_kick()

    def _before_await(self, future: _core.HostFuture) -> None:
        if not self._undecided:
            return
        task = asyncio.current_task()
        if task is None:
            return
        owner = future.owner()
        for index, holder in list(self._undecided.items()):
            if holder is task and owner != self._tokens[index]:
                self._release(index, task)

    def _halt_member(self, machine: Machine) -> None:
        """`Machine.halt()` on a member: release it from its goal's stop."""
        index = self._index[machine.name]
        if self._free:
            raise MachineBusy(f"the linked system runs freely: halt the system, not {machine.name}")
        if index in self._goals:
            raise MachineBusy(f"{machine.name} has a goal pending: cancel its task")
        if index in self._undecided:
            self._release(index, self._undecided[index])

    def _watchdog(self, index: int, task: asyncio.Task[object], stop: Stop) -> None:
        if self._closed or self._undecided.get(index) is not task:
            return
        key = (index, id(task))
        if key in self._warned:
            return
        self._warned.add(key)
        _LOG.warning(
            "%s waits at its goal's stop %s for %.0f s: the system waits for task %s to finish,"
            " await a popcorn future of another machine or call %s.halt()",
            self._names[index],
            stop,
            _WATCHDOG,
            task.get_name(),
            self._names[index],
        )

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
        self._free = False
        if self._flight is None and self._failed is None:
            with contextlib.suppress(RuntimeError):
                self._handle.finish()
        self._handle.close()
        self._unlink()
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


class UartTap:
    """Both directions of one link of a `LinkedSystem`, `system.tap("a.USART1")`: every frame
    on the line with its system times, and frames injected, replaced or dropped by number.

    The tap gets a frame once it ended, its stop-bit end at or before a point where the system
    paused, so the frames it holds at a pause, `end` at or before that pause's point, do not
    depend on the quantum, the slicing or the wall timing. After `close()` it holds every frame
    that ended by the last pause. It keeps up to 1 Mi frames per direction not read yet, about
    380 bytes each, the oldest dropped and counted in `dropped`: take them with `frames()`.
    Reading never changes what the receiver gets.

    `towards` names the receiving end of a direction, `"<member>.<UART>"`.
    """

    __slots__ = (
        "__weakref__",
        "_buffers",
        "_dropped_base",
        "_ends",
        "_lock",
        "_overflow",
        "_system",
        "_wires",
    )

    def __init__(self, system: LinkedSystem, wired: _LinkWires) -> None:
        """Wrap a link of `system`. Use `LinkedSystem.tap` instead."""
        self._system = system
        self._ends = wired.ends
        self._wires = wired.wires
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
        start or the last `LinkedSystem.restore`."""
        return self._dropped_total() - self._dropped_base

    def _dropped_total(self) -> int:
        handle = self._system._handle  # pyright: ignore[reportPrivateUsage]
        return self._overflow + sum(handle.tap_dropped(wire) for wire in self._wires)

    def frames(self, *, towards: str | None = None) -> list[TapFrame]:
        """Take every frame the tap holds, towards `towards` only when given, oldest first.
        Never runs the system, allowed stopped and running, from any thread.

        At a pause, or after `close()`, it takes exactly the frames ended by that point and
        not taken yet, whatever the quantum, the slicing or the wall timing. While the system
        runs, the tap fills at each barrier, so how many frames a call takes depends on wall
        timing. Each frame's content never does.

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
        quantum, the slicing or the wall timing.

        Stopped or paused, they are checked at the call: `at` must be at or after the
        receiver's system time, and no frame may overlap one already on the receiver's line.
        While the system runs, from a `running()` body or another thread, `at` must be at or
        after the reach of the deliveries done so far, which depends on wall timing, and an
        overlap is found later: the system stops with an error at the later frame's start,
        a time fixed by the frames.

        Raises:
            TypeError: `data` is no bytes, `at` no VirtualTime or `towards` no str.
            ValueError: `data` empty, `towards` no end of this link, `at` too early, or, stopped
                or paused, an overlap. The message names the bound.
            EmulationError: The system was closed, or stopped on an error before.
            RuntimeError: Called from a callback of a machine.
        """
        frames = _data_frames(data)
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

    def replace(self, index: int, data: bytes, *, towards: str) -> None:
        """Give the sender's frames numbered `index` on, one per byte, new data before they
        start: the receiver and the tap see the new data at the frames' own times. Armed at
        any time, from a `running()` body too: while the sender runs, whether the edit comes
        before the frame starts depends on wall timing, its effect does not.

        Raises:
            TypeError: `index` is no int, `data` no bytes, or `towards` no str.
            ValueError: `index` negative, `data` empty, `towards` no end of this link, a frame
                it covers started already, `posted`, or it overlaps an edit armed before.
            EmulationError: The system was closed, or stopped on an error before.
        """
        frames = _data_frames(data)
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
        _check_count("n", n, 1)
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
        _check_count("index", index, 0)
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
                )
                for record in records
            )

    def __repr__(self) -> str:
        return f"UartTap({self._ends[0]} <-> {self._ends[1]})"


def _data_frames(data: bytes) -> list[int]:
    if not isinstance(cast(object, data), bytes | bytearray | memoryview):
        raise TypeError(f"data must be bytes, not {type(data).__name__}")
    frames = list(bytes(data))
    if not frames:
        raise ValueError("data must hold at least one byte")
    return frames


def _check_count(name: str, value: int, least: int) -> None:
    if isinstance(cast(object, value), bool) or not isinstance(cast(object, value), int):
        raise TypeError(f"{name} must be an int, not {type(value).__name__}")
    if value < least:
        raise ValueError(f"{name} must be at least {least}, not {value}")


def _unmark(marked: list[tuple[UartPorts, str]]) -> None:
    """Give a closed or collected system's UARTs back to their streams and ptys."""
    for ports, unit in marked:
        ports.linked.discard(unit)


def _on_loop(loop: asyncio.AbstractEventLoop) -> bool:
    try:
        return asyncio.get_running_loop() is loop
    except RuntimeError:
        return False


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


def _check_every(every: VirtualDuration, for_: VirtualDuration | None) -> None:
    for name, given in (("every", every), ("for_", for_)):
        if given is not None and not isinstance(cast(object, given), VirtualDuration):
            raise TypeError(f"{name} must be a VirtualDuration, not {type(given).__name__}")
    if every.ps <= 0:
        raise ValueError("every must be above 0")
    if for_ is not None and for_.ps < 0:
        raise ValueError("for_ must not be negative")


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
