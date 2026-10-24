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
from typing import TYPE_CHECKING, Any, ClassVar, Literal, cast

from popcorn import _core
from popcorn._enums import ResetKind, StopReason
from popcorn._errors import ConfigError, EmulationError, MachineBusy, MachineRunning
from popcorn._host import awaited, watch_awaits
from popcorn._machine_description import (
    MachineDescription,
    _FrozenMap,  # pyright: ignore[reportPrivateUsage]
)
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
_OPEN_END = (1 << 63) - 1

type LineState = Literal["idle", "break"]
"""The state a cut line is held in: `"idle"` keeps it high, `"break"` holds it low."""


@dataclass(frozen=True)
class UartLink:
    """A UART link of a `System`, both directions, between two members or between two
    UARTs of one member.

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
        parity: Declared parity, "none", "even" or "odd": the parity bit of injected frames.
            A sender whose USART frames with another parity, CR1.PCE and PS, is warned about
            as for another baud. Each USART computes the parity bit of the frames it sends and
            checks the bit of the frames it receives, a mismatch setting ISR.PE, so a link
            whose ends match its declared parity carries correct bits.
    """

    a: str
    b: str
    latency: VirtualDuration
    baud: int
    data_bits: int = 8
    stop_bits: float = 1.0
    parity: Literal["none", "even", "odd"] = "none"


@dataclass(frozen=True, slots=True)
class SystemDescription:
    """Members and links of a linked system, as data: what `System` runs.

    A value, as `MachineDescription` is: frozen, hashable, equal by content, holding machine
    descriptions and never a machine. Declaring one reads no file, so a conftest imports it
    before the firmware exists. `machines`, `boot` and `link` build the runtime. The pytest
    plugin's `@popcorn.pytest.system` fixtures boot each member from its own description's boot
    cache.

    Each member runs on its own host thread. The members meet at a barrier every `quantum` of
    system time, and after it each receiver's thread drains the frames sent to it, senders in
    member order, then links in `links` order. The Python callbacks of different members run
    at once, each on its member's thread.

        SYSTEM = popcorn.SystemDescription(
            members={"ctl": CONTROLLER, "r1": RESPONDER, "r2": RESPONDER},
            links=[
                popcorn.UartLink("ctl.UART4", "r1.USART3", latency=us(20), baud=460_800),
                popcorn.UartLink("ctl.USART3", "r2.USART3", latency=us(20), baud=460_800),
            ],
        )

    Attributes:
        members: Each member's machine description, by member name, in member order. A name is a
            `Machine` name: not empty, no whitespace, no dot. Members may share a machine
            description: each is its own machine. A member's config and strictness are part of its
            machine description, `CONTROLLER.config("audio")`, `RESPONDER.replace(strict=True)`.
        links: The links, at least one. Their order is part of the system: it fixes the order
            of frames arriving at one receiver at the same instant on several links.
        offsets: System time of each member's local time at the start, 0 for an absent one.
        goal_cap: Virtual time a member goal may run from its start, None for no cap, as
            `System(goal_cap=)`.
        on_member_reset: What a firmware or watchdog reset of a running member does, as
            `System(on_member_reset=)`.

    Raises:
        ConfigError: A check fails, its key naming the field: `members` empty, `members.<name>`
            a bad name or no MachineDescription, `links` empty, `links[i].a` or `.b` naming no
            member's
            peripheral as `"<member>.<UART>"`, or a UART in two links, `links[i].latency`,
            `.baud`, `.data_bits`, `.stop_bits` or `.parity` out of range, `offsets.<name>`
            naming no member or below 0, `goal_cap` not above 0, or `on_member_reset` neither
            "reset" nor "stop". Whether a link end is a UART of the member's part is checked by
            `link`, which has the description.
    """

    members: Mapping[str, MachineDescription]
    links: Sequence[UartLink]
    offsets: Mapping[str, VirtualDuration] = field(default_factory=_FrozenMap[str, VirtualDuration])
    goal_cap: VirtualDuration | None = None
    on_member_reset: Literal["reset", "stop"] = "reset"

    def __post_init__(self) -> None:
        object.__setattr__(self, "members", _FrozenMap(self.members))
        object.__setattr__(self, "links", tuple(self.links))
        object.__setattr__(self, "offsets", _FrozenMap(self.offsets))
        if not self.members:
            raise ConfigError(
                "members: a system needs a member", key="members", expected="a member"
            )
        for name, project in cast(Mapping[object, object], self.members).items():
            key = f"members.{name}"
            if not isinstance(name, str) or not _is_machine_name(name):
                raise ConfigError(
                    f"{key}: {name!r} is no machine name",
                    key=key,
                    expected="a name, not empty, without whitespace or a dot",
                )
            if not isinstance(project, MachineDescription):
                raise ConfigError(
                    f"{key}: a {type(project).__qualname__}, not a MachineDescription",
                    key=key,
                    expected="a popcorn.MachineDescription",
                )
        if not self.links:
            raise ConfigError("a linked system needs a link", key="links", expected="a link")
        used: set[str] = set()
        for index, link in enumerate(cast(tuple[object, ...], self.links)):
            if not isinstance(link, UartLink):
                raise ConfigError(
                    f"links[{index}]: a {type(link).__qualname__}, not a UartLink",
                    key=f"links[{index}]",
                    expected="a popcorn.UartLink",
                )
            _check_line(index, link)
            for side in ("a", "b"):
                self._check_end(index, side, used)
        _offsets(list(self.members), self.offsets)
        cap = cast(object, self.goal_cap)
        if cap is not None and (not isinstance(cap, VirtualDuration) or cap.ps <= 0):
            raise ConfigError(
                f"goal_cap: {cap!r} is no duration above 0",
                key="goal_cap",
                expected="None or a VirtualDuration above 0",
            )
        if self.on_member_reset not in ("reset", "stop"):
            raise ConfigError(
                f"on_member_reset: {self.on_member_reset!r} is neither 'reset' nor 'stop'",
                key="on_member_reset",
                expected="'reset' or 'stop'",
            )

    def _check_end(self, index: int, side: str, used: set[str]) -> None:
        """Check end `side` of `links[index]` and mark it used."""
        key = f"links[{index}].{side}"
        end = cast(object, getattr(self.links[index], side))
        if not isinstance(end, str):
            raise ConfigError(
                f"{key}: a {type(end).__qualname__}, not a str",
                key=key,
                expected="'<member>.<UART>'",
            )
        name, dot, unit = end.partition(".")
        if not dot or not unit or name not in self.members:
            raise ConfigError(
                f"{key}: {end!r} names no member UART", key=key, expected="'<member>.<UART>'"
            )
        if end in used:
            raise ConfigError(f"{key}: {end} is in two links", key=key, expected="a free UART")
        used.add(end)

    @property
    def quantum(self) -> VirtualDuration:
        """The quantum Q, the smallest link latency: members meet every Q of system time."""
        return min(link.latency for link in self.links)

    def replace(self, **changes: object) -> SystemDescription:
        """A copy with `changes` applied, as `dataclasses.replace`, checked again."""
        return replace(self, **changes)

    def machines(self) -> dict[str, Machine]:
        """A new machine per member, `MachineDescription.machine(name=<member>)`, not reset.

        Raises:
            Exception: As `MachineDescription.machine`.
        """
        return {name: project.machine(name=name) for name, project in self.members.items()}

    async def boot(self) -> System:
        """Build `machines`, boot each from reset as `MachineDescription.boot_machine` does, or only
        reset it when its machine description has no `boot.until`, one after the other, then `link`
        them. For scripts: the pytest plugin's `@popcorn.pytest.system` fixtures boot from the boot
        cache instead.

        Raises:
            Exception: As `machines`, `MachineDescription.boot_machine` and `link`.
        """
        machines = self.machines()
        for name, machine in machines.items():
            project = self.members[name]
            if project.boot.until is None:
                machine.reset()
            else:
                await project.boot_machine(machine)
        return self.link(machines)

    def link(
        self,
        machines: Mapping[str, Machine],
        *,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
    ) -> System:
        """The `System` of this system over `machines`, booted any way, each standing
        where its system time starts: `offsets` gives that time.

        Args:
            machines: A parked machine per member, by member name, named as the member.
            slice_instructions: As `System(slice_instructions=)`.
            slice_wall_budget: As `System(slice_wall_budget=)`.

        Raises:
            ConfigError: `machines` misses a member or names another, at `members.<name>`, or
                as `System`.
            Exception: As `System`.
        """
        unmatched = sorted(set(self.members).symmetric_difference(machines))
        if unmatched:
            name = unmatched[0]
            raise ConfigError(
                f"members.{name}: a machine of no member, or a member with no machine",
                key=f"members.{name}",
                expected="one machine per member",
            )
        return System(
            {name: machines[name] for name in self.members},
            self.links,
            offsets=self.offsets,
            on_member_reset=self.on_member_reset,
            goal_cap=self.goal_cap,
            slice_instructions=slice_instructions,
            slice_wall_budget=slice_wall_budget,
        )


@dataclass(frozen=True)
class SystemStop:
    """Why and where a `System.run` ended.

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
    """A `System`'s whole state at a stop, `System.snapshot`, restorable into it
    any number of times with `System.restore`. In memory only.

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
        data: Data bits, 0 for a break, the parity bit stripped as in `Frame.data`.
        index: Number of the frame among its sender's frames in this direction, from 0 at
            system start, or among the frames injected in this direction for an injected one:
            the index `UartTap.replace` and `drop` take for a sender's frame.
        start: Start-bit edge at the sender, at the receiver for an injected frame.
        end: Stop-bit end at the sender, the abort time for a frame the sender aborted, at the
            receiver for an injected frame. A frame a cut made lost keeps its stop-bit end.
        arrival: When the receiver sees the frame end: `end` plus the link's latency, `end` for
            an injected frame.
        towards: The receiving end, `"<member>.<UART>"`.
        injected: Whether `UartTap.inject` put it on the line.
        brk: Whether it is a break.
        lost: Whether the sender aborted it, its UART disabled mid-frame, or a cut made it
            cross the cut's start or end.
        dropped: Whether an armed `UartTap.drop` kept it off the line: the receiver saw an idle
            line.
        cut: Whether a line cut touched it, `UartTap.cut`: it starts inside the cut, or crosses
            the cut's start, also `lost` then. A dropped frame is never `cut`.
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
    cut: bool


@dataclass(frozen=True)
class _LinkWires:
    ends: tuple[str, str]
    wires: tuple[int, int]
    link: int


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


class System:
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

    A member whose goal stops waits exactly at its stop, parked, and the system waits with it. The
    task that posted the goal holds it. `gather`, `TaskGroup`, `shield` and `ensure_future` post
    from a child task of their own, which releases the member as soon as it ends. The member is
    released when its holder finishes, or awaits a popcorn future of another machine, or at the next
    system `run`, `wait_for`, `start`, `snapshot` or `restore`. A new goal on it before that starts
    exactly at its stop, also after unrelated awaits such as `asyncio.sleep`. A released member runs
    its core from its stop in the next system `run`. While member goals run, members without a goal
    are halted: their cores execute nothing, their clocks, peripherals and DMA follow system time,
    their DBGMCU freeze bits apply. `member.halt()` halts a member where it stands, released from
    its goal's stop or not, one that never had a goal too: its core executes nothing in system runs
    either, until its next goal or reset. A task that keeps a member parked while it awaits
    something only a peer's goal can complete waits forever, and so does one that awaits its own
    member's future that needs time to pass, such as a write at a later time: nothing runs while the
    member is parked. After 5 s a warning names the member and the task, `member.halt()` releases
    it, halted. Cancelling a goal's task withdraws the goal at the next pause, where the member then
    stands, a point that depends on wall timing. A goal posted while other goals run is admitted at
    the next pause, also wall-dependent. An error a member raises, from a handler or a model, stops
    the system: every pending goal fails with that error, noted with the member and its system time,
    and a goal stopped before it keeps its stop.

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
    `snapshot` and `restore` of a member raise MachineBusy, `halt` halts it, and `reset` goes
    through `System.reset`. `snapshot` and `restore` take and give back the whole
    system's state, every member included, while it is stopped. Its
    Python callbacks run on its own thread, concurrently with other members' callbacks: Python
    state they share sees an order that depends on thread timing, so keep callback state per
    member and couple members through links. The system owns the members' UARTs named in links:
    frames each transmits still reach its own `port`, whose `write` raises RuntimeError until
    `close()`. `tap` shows the frames on a link, injects, replaces or drops frames, and cuts
    the line for a window of system time.

    The system holds its members until `close()`, also run by `with`.
    """

    _sync: ClassVar[Literal["barrier", "clocks"]] = "barrier"

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
        "_halts",
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
                member, a UART in two links or bridged to a pty, or whose CTS input
                `UartStream.set_cts` left not ready or with a change pending, or whose model
                takes no frames from a wire, such as a plugin UART implementing only
                `periph::uart_port`, `links[i].a` or `links[i].b`,
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
        self._handle.set_sync(type(self)._sync)
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
        self._halts: set[int] = set()
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
        word_bits = _check_line(index, link)
        ends = [self._end(index, side, end, used) for side, end in (("a", link.a), ("b", link.b))]
        wires: list[int] = []
        for side, (sender, sender_link, _), (receiver, receiver_link, unit) in (
            ("b", ends[0], ends[1]),
            ("a", ends[1], ends[0]),
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
            try:
                wires.append(self._handle.add_wire(spec, sender_link, receiver_link))
            except ConfigError as error:
                if error.key != "wire":
                    raise
                key = f"links[{index}].{side}"
                end = f"{self._names[receiver]}.{unit}"
                raise ConfigError(
                    f"{key}: {end} is not {error.expected}",
                    file=error.file,
                    key=key,
                    expected=error.expected,
                ) from error
        self._links.append(
            _LinkWires(
                (
                    f"{self._names[ends[0][0]]}.{ends[0][2]}",
                    f"{self._names[ends[1][0]]}.{ends[1][2]}",
                ),
                (wires[0], wires[1]),
                self._handle.add_link(wires[0], wires[1]),
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
        if not native.cts_settled_ready():
            raise ConfigError(
                f"{key}: {name}.{serving} has its CTS input not ready or a set_cts change"
                " pending, which a link could never release",
                key=key,
                expected="a UART whose CTS input is ready, with no change pending",
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
        """Run every member's core that is not halted to system time `until`, or `for_` past
        the last target. A member `halt()` froze stays halted until its next goal or reset.

        `for_` counts from the target of the previous TIME stop, else from the largest offset,
        the latest member's system time at construction, so ten `run(for_=ms(10))` equal one
        `run(for_=ms(100))`. Never from where a member stop left the members: that position
        depends on the quantum. Each member stops
        at the first block boundary at or after the target, an idle or halted one exactly at it.
        A member stop on the way, FAULT, a breakpoint or a reset under `on_member_reset="stop"`,
        ends the run there: the member waits at its stop, the others where the run left them.
        The next `run` releases it halted: its core executes nothing in system runs until its
        next goal or reset. A member released from its own goal's stop runs on from that stop.

        A run pauses IDLE once every member sleeps with nothing due and every link is empty. A
        `line="break"` cut of a link, `UartTap.cut`, makes its hold and release due: the pause
        then comes only after its release and the receiver's IDLE after it.

        Each member's `last_stop` becomes its stop in the result's `stops`, None for a member a
        member stop left where it stood, and None for every member when the run ends without a
        result, cancelled or raising.

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
        self._set_last_stops({})
        stop = self._system_stop(await self._resume(target), target)
        self._set_last_stops(stop.stops)
        return stop

    def _set_last_stops(self, stops: Mapping[str, Stop]) -> None:
        """Each member's `last_stop` after a system run: its stop in `stops`, None for a member
        the run left where it stood."""
        for name, machine in self._machines.items():
            machine._last_stop = stops.get(name)  # pyright: ignore[reportPrivateUsage]

    async def wait_for(
        self,
        predicate: Callable[[System], bool],
        *,
        every: VirtualDuration,
        for_: VirtualDuration | None = None,
    ) -> SystemStop:
        """Run every member's core that is not halted until `predicate(system)` holds at a
        check point.

        The check points are system times `base + k * every`, k from 1, where `base` is the
        target of the previous TIME stop, else the largest offset, as for `run(for_=)`. At each
        one the system makes a TIME stop, every member parked at it, and `predicate` runs on
        the caller's thread with sync access to every member. The stop is the first check point
        where it holds, whatever the quantum, the slicing or the wall timing: the predicate is
        never asked between check points. Each check point is a TIME target, so a later
        `run(for_=)` or `wait_for` counts from the stop.

        With `for_`, the wait ends at `base + for_` with a TIME stop if `predicate` held at no
        check point before it, and is not asked there. A member stop or an IDLE pause on the way
        ends the wait as it ends `run`. Each member's `last_stop` is its stop of the last run,
        as after `run`: the TIME stop of the last check point, also when `predicate` raises.

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
        others go on, and `halt()` raises it, and that stop becomes its `last_stop`. Every
        member's `last_stop` is None from the start on, until a member stop or one of its waits
        gives it one. When every member sleeps with nothing due the
        system sleeps until a command, a stamp, an injection, a cut or a pty byte wakes a
        member. A `line="break"` cut ahead counts as due until its release and the receiver's
        IDLE after it, `UartTap.cut`.
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
                        index, None, None, None, None, None, None, True, self._goal_cap, None
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
        self._set_last_stops({})
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

        The system sleeps as in `start()`: a `line="break"` cut ahead, `UartTap.cut`, keeps it
        awake until its release and the receiver's IDLE after it.

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
                self._handle.release(index, False)

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
            ConfigError: `kind` is `ResetKind.PERIPHERAL`, which only an RCC reset bit applies.
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

        A member still held at a stop is released first, as the next `run` releases it: halted
        when the stop is a member stop the last run found, else a goal posted on it starts at
        that stop and a later `run` runs its core from there. The snapshot keeps which.

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

    def _quiet(self) -> bool:
        """Whether `_require_quiet` would pass, without raising."""
        return not (
            self._closed
            or self._failed is not None
            or self._flight is not None
            or self._free
            or self._halting
            or self._opens
            or self._goals
        )

    def _release_undecided(self) -> None:
        """Release every member held at a stop: a member stop a system run found halts its member,
        a goal's holder lets it run on from its stop."""
        for index, holder in sorted(self._undecided.items()):
            self._handle.release(index, holder is None)
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
                self._handle.release(pause.member, False)
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
            for index in sorted(self._halts):
                if index not in self._goals:
                    self._handle.release(index, True)
                    self._undecided.pop(index, None)
        else:
            for index in sorted(self._waits):
                self._fail_waits(index)
        self._withdrawals.clear()
        self._rebuilds.clear()
        self._halts.clear()
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
            self._handle.release(index, self._free)
            if self._free:
                self._ends.append((index, stop))
                stopper = self._machines[self._names[index]]
                stopper._last_stop = stop  # pyright: ignore[reportPrivateUsage]
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
                index, None, None, None, None, None, None, True, self._goal_cap, None
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
        returning: Callable[[], tuple[int, int]] | None = None,
    ) -> Stop:
        """Run a goal of member `machine`, parked at its stop once delivered: see the class.
        Inside the member's own `running()` it joins the run instead, `_join`, except a `run`,
        refused there as on a lone machine's free run. `returning` resolves a frame-return
        target, `(return address, sp)`, once the member is parked, right before the post."""
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
                returning=returning,
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
        returning: Callable[[], tuple[int, int]] | None = None,
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
                returning=returning,
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
        returning: Callable[[], tuple[int, int]] | None,
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
        until_return = None if returning is None else returning()
        task = asyncio.current_task()
        until_ps: int | None = None
        if for_ is not None:
            until_ps = self._handle.time_of_ps(index) + for_.ps
        elif deadline is not None:
            base = self._handle.time_of_ps(index) - machine.time.ps
            until_ps = base + deadline.ps
        future = self._handle.post_goal(
            index,
            until,
            budget,
            exact,
            until_ps,
            stop_when,
            gate,
            open_,
            self._goal_cap,
            until_return,
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
            self._handle.release(index, False)
        self._schedule_kick()

    def _release(self, index: int, owner: asyncio.Task[object] | None) -> None:
        """Release member `index`, parked at its goal's stop, when `owner` still holds it, None
        for any holder. It runs on from its stop in later system runs."""
        if index not in self._undecided:
            return
        if owner is not None and self._undecided[index] is not owner:
            return
        del self._undecided[index]
        if self._closed or self._failed is not None:
            return
        if index in self._handle.undecided():
            self._handle.release(index, False)
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
        """`Machine.halt()` on a member: halt it where it stands, released from its goal's stop."""
        index = self._index[machine.name]
        if self._free:
            raise MachineBusy(f"the linked system runs freely: halt the system, not {machine.name}")
        if self._system_run:
            raise MachineBusy(f"the linked system runs a system run: halt {machine.name} after it")
        if index in self._goals:
            raise MachineBusy(f"{machine.name} has a goal pending: cancel its task")
        if self._closed or self._failed is not None:
            self._undecided.pop(index, None)
            return
        if self._flight is not None:
            self._halts.add(index)
            return
        self._handle.release(index, True)
        self._undecided.pop(index, None)
        self._schedule_kick()

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
                resets=machine._handle.resets(),  # pyright: ignore[reportPrivateUsage]
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

    async def _teardown(self) -> None:
        """Halt a free run, raising what `halt()` raises, then close and wait for the last
        flight to take the closed pause, so nothing on the loop holds the system after."""
        try:
            await self._halt_free()
        finally:
            self.close()
            while (flight := self._flight) is not None and not flight.done():
                with contextlib.suppress(Exception):
                    await asyncio.shield(flight)

    def __enter__(self) -> System:
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
        return f"System({', '.join(self._names)})"


class UartTap:
    """Both directions of one link of a `System`, `system.tap("a.USART1")`: every frame
    on the line with its system times, and frames injected, replaced or dropped by number.

    The tap gets a frame once it ended, its stop-bit end at or before a point where the system
    paused, so the frames it holds at a pause, `end` at or before that pause's point, do not
    depend on the quantum, the slicing or the wall timing. After `close()` it holds every frame
    that ended by the last pause. It keeps up to 1 Mi frames per direction not read yet, about
    380 bytes each, the oldest dropped and counted in `dropped`: take them with `frames()`.
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
      system times, `System.time_of`. After a run a member stop ended, the other members stand
      where the run left them, a position that depends on the quantum, and so does now. `cut`
      accepts a start from now on.
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

    def __init__(self, system: System, wired: _LinkWires) -> None:
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
        quantum, the slicing or the wall timing. With a declared parity, each frame carries
        its parity bit in the MSB of its word, in place of that data bit.

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
        _check_time("start", start)
        _check_time("end", end)
        if end.ps <= start.ps:
            raise ValueError(f"cut [{start}, {end}): end must be after start")
        low = _line_low(line)
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
        low = _line_low(line)
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
        member calls first.

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
                    cut=record.cut,
                )
                for record in records
            )

    def __repr__(self) -> str:
        return f"UartTap({self._ends[0]} <-> {self._ends[1]})"


def _check_time(name: str, value: VirtualTime) -> None:
    if not isinstance(cast(object, value), VirtualTime):
        raise TypeError(f"{name} must be a VirtualTime, not {type(value).__name__}")


def _line_low(line: LineState) -> bool:
    if not isinstance(cast(object, line), str):
        raise TypeError(f"line must be a str, not {type(line).__name__}")
    if line not in ("idle", "break"):
        raise ValueError(f"line must be 'idle' or 'break', not {line!r}")
    return line == "break"


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


def _check_line(index: int, link: UartLink) -> int:
    """Check the line values of `links[index]`, return its word bits, parity bit included."""
    latency = cast(object, link.latency)
    if not isinstance(latency, VirtualDuration) or latency.ps <= 0:
        raise ConfigError(
            f"links[{index}].latency: {link.latency!r} is no duration above 0",
            key=f"links[{index}].latency",
            expected="a VirtualDuration above 0",
        )
    baud, data_bits, parity, stop_bits = cast(
        tuple[object, object, object, object],
        (link.baud, link.data_bits, link.parity, link.stop_bits),
    )
    integral = isinstance(data_bits, int) and not isinstance(data_bits, bool)
    word_bits = cast(int, data_bits) + (0 if parity == "none" else 1) if integral else 0
    for name, refused, expected in (
        (
            "baud",
            not isinstance(baud, int) or isinstance(baud, bool) or baud <= 0,
            "a baud above 0",
        ),
        ("parity", not isinstance(parity, str) or parity not in _PARITY, "'none', 'even' or 'odd'"),
        ("data_bits", word_bits not in (7, 8, 9), "7 to 9 data bits, the parity bit included"),
        (
            "stop_bits",
            not isinstance(stop_bits, int | float)
            or isinstance(stop_bits, bool)
            or stop_bits not in _STOP_BITS,
            "0.5, 1, 1.5 or 2 stop bits",
        ),
    ):
        if refused:
            key = f"links[{index}].{name}"
            raise ConfigError(
                f"{key}: {getattr(link, name)!r} is no UART line value", key=key, expected=expected
            )
    return word_bits


def _is_machine_name(name: str) -> bool:
    from popcorn._machine import check_machine_name

    try:
        check_machine_name(name)
    except ValueError:
        return False
    return True
