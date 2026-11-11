from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
from typing import TYPE_CHECKING, Literal, cast

from popcorn._enums import StopReason
from popcorn._errors import (
    ConfigError,
)
from popcorn._machine_description import (
    MachineDescription,
    _FrozenMap,  # pyright: ignore[reportPrivateUsage]
)
from popcorn._snapshot import Snapshot
from popcorn._stop import Stop
from popcorn._system.checks import is_machine_name, offsets_of
from popcorn._system.links import I2CBus, SpiBus, UartLink, check_medium, medium_of
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine
    from popcorn._system.system import System


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
        links: The links, `UartLink`, `SpiBus` or `I2CBus`, at least one. Their order is part of the
            system: it fixes the order of frames arriving at one receiver at the same instant on
            several links.
        offsets: System time of each member's local time at the start, 0 for an absent one.
        goal_cap: Virtual time a member goal may run from its start, None for no cap, as
            `System(goal_cap=)`.
        on_member_reset: What a firmware or watchdog reset of a running member does, as
            `System(on_member_reset=)`.

    Raises:
        ConfigError: A check fails, its key naming the field: `members` empty, `members.<name>`
            a bad name or no MachineDescription, `links` empty, `links[i]` neither `UartLink`,
            `SpiBus` nor `I2CBus`, `links[i].a` or `.b` naming no member's peripheral as
            `"<member>.<UART>"`, `links[i].master` or `links[i].slaves[<chip select>]` naming
            none as `"<member>.<SPI>"`, a chip select that is not `"<master member>.<pin>"` or
            `"<master member>.NSS"`, a slave on the master's member, `links[i].ports` with no
            target, `links[i].ports[j]` naming none as `"<member>.<I2C>"` or a target on the
            controller's member, or a UART, SPI or I2C in two links, `links[i].latency`,
            `.baud`, `.data_bits`, `.stop_bits` or `.parity` out of range, `offsets.<name>`
            naming no member or below 0, `goal_cap` not above 0, or `on_member_reset` neither
            "reset" nor "stop". Whether a link end is a UART, SPI or I2C of the member's part is
            checked by `link`, which has the description.
    """

    members: Mapping[str, MachineDescription]
    links: Sequence[UartLink | SpiBus | I2CBus]
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
            if not isinstance(name, str) or not is_machine_name(name):
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
            for key, end, kind in check_medium(index, medium_of(index, link)):
                self._check_end(key, end, kind, used)
        offsets_of(list(self.members), self.offsets)
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

    def _check_end(self, key: str, end: object, kind: str, used: set[str]) -> None:
        """Check one end of a link, a `"<member>.<kind>"` string, and mark it used."""
        if not isinstance(end, str):
            raise ConfigError(
                f"{key}: a {type(end).__qualname__}, not a str",
                key=key,
                expected=f"'<member>.<{kind}>'",
            )
        name, dot, unit = end.partition(".")
        if not dot or not unit or name not in self.members:
            raise ConfigError(
                f"{key}: {end!r} names no member {kind}",
                key=key,
                expected=f"'<member>.<{kind}>'",
            )
        if end in used:
            raise ConfigError(f"{key}: {end} is in two links", key=key, expected=f"a free {kind}")
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
        from popcorn._system.system import System

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
