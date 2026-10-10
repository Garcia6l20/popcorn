"""NFR-ISO-1: machines running at the same wall time, each on its own host thread, give the same
virtual results as each machine run alone, and as machines driven one at a time."""

from __future__ import annotations

import asyncio
import hashlib
import struct
import threading
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import cast

import popcorn
import pytest
from popcorn._refs import Pending
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import ROOT

EXAMPLES = ROOT / "examples" / "stm32u575"
USART_ELF = EXAMPLES / "usart" / "prebuilt" / "usart_echo.elf"
IWDG_ELF = EXAMPLES / "iwdg" / "prebuilt" / "iwdg_refresh.elf"
LINES = (b"alpha\r\n", b"bravo charlie\r\n", None)
WRITE_AT = ms(5)
LATE_LINE = b"delta\r\n"
LATE_AT = ms(30)
STEPS = 50
TICK = "HAL_IncTick"
RESTORE_CHANGES = (("events",), ("arch", "data"), ("generation",))


@dataclass(frozen=True)
class Digest:
    time: VirtualTime
    instructions: int
    registers: tuple[int, ...]
    ram: str
    state: str
    restorable: str
    pending_events: int

    def restored(self) -> tuple[VirtualTime, int, tuple[int, ...], str, str]:
        """The digest without what a restore of a machine's own snapshot changes today: the
        event queue, the arch data and the generation of the snapshot."""
        return (self.time, self.instructions, self.registers, self.ram, self.restorable)


@dataclass
class Member:
    machine: popcorn.Machine
    start: VirtualTime
    written: Pending[VirtualTime] | None
    late: Pending[VirtualTime] | None = None
    restorable: bool = False


Targets = Sequence[VirtualDuration]
Handler = Callable[[popcorn.Call], popcorn.Return]


def proceed(call: popcorn.Call) -> popcorn.Return:
    return call.proceed()


def bson(data: bytes, at: int = 0) -> dict[str, object]:
    """The BSON document at `at`, with the element types snapshots use."""
    (size,) = struct.unpack_from("<i", data, at)
    end = at + size - 1
    at += 4
    found: dict[str, object] = {}
    while at < end:
        kind = data[at]
        name_end = data.index(0, at + 1)
        name = data[at + 1 : name_end].decode()
        at = name_end + 1
        value: object
        if kind in (0x03, 0x04):
            value = bson(data, at)
            at += struct.unpack_from("<i", data, at)[0]
        elif kind == 0x05:
            (length,) = struct.unpack_from("<i", data, at)
            value = data[at + 5 : at + 5 + length]
            at += 5 + length
        elif kind == 0x02:
            (length,) = struct.unpack_from("<i", data, at)
            value = data[at + 4 : at + 3 + length].decode()
            at += 4 + length
        elif kind == 0x01:
            (value,) = struct.unpack_from("<d", data, at)
            at += 8
        elif kind in (0x09, 0x11, 0x12):
            (value,) = struct.unpack_from("<q", data, at)
            at += 8
        elif kind == 0x10:
            (value,) = struct.unpack_from("<i", data, at)
            at += 4
        elif kind == 0x08:
            value = data[at] != 0
            at += 1
        elif kind == 0x0A:
            value = None
        else:
            raise ValueError(f"BSON element type {kind:#x} at {name}")
        found[name] = value
    return found


def feed(update: Callable[[bytes], object], value: object, path: tuple[str, ...] = ()) -> None:
    if isinstance(value, dict):
        for key, inner in cast(dict[str, object], value).items():
            if (*path, key) in RESTORE_CHANGES:
                continue
            update(key.encode() + b"\0")
            feed(update, inner, (*path, key))
    elif isinstance(value, bytes):
        update(len(value).to_bytes(8, "little") + value)
    else:
        update(repr(value).encode())


def digest(m: popcorn.Machine, restorable: bool = False) -> Digest:
    ram = hashlib.sha256()
    for region in m.description.regions:
        if region.name.startswith("SRAM"):
            ram.update(m.mem.read(region.start, region.size))
    saved = m.snapshot().to_bytes()
    trimmed = hashlib.sha256()
    if restorable:
        feed(trimmed.update, bson(saved))
    return Digest(
        m.time,
        m.instructions,
        tuple(getattr(m.reg, name) for name in m.reg.names()),
        ram.hexdigest(),
        hashlib.sha256(saved).hexdigest(),
        trimmed.hexdigest(),
        m.pending_events,
    )


def members(
    u575: popcorn.MachineDescription,
    slice_instructions: int = 1_000_000,
    ticks: Sequence[Handler] = (),
) -> list[Member]:
    """A and B over the usart example, each sent its own line, and C over the iwdg example, all
    at `main`, each with its handler of `ticks` intercepting the HAL tick. Intercepts are part
    of the snapshot, so machines compared have the same ones."""
    made: list[Member] = []
    for index, (elf, line) in enumerate(zip((USART_ELF, USART_ELF, IWDG_ELF), LINES, strict=True)):
        m = popcorn.Machine(
            u575, image=popcorn.Image.elf(elf), slice_instructions=slice_instructions
        )
        m.reset()
        stop = asyncio.run(m.run(until="main", budget=1_000_000))
        assert stop.reason is popcorn.StopReason.ADDRESS, stop
        if ticks:
            m.intercept(TICK, ticks[index])
        port = m.periph.USART1.port
        written = None if line is None else port.write(line, at=m.time + WRITE_AT)
        made.append(Member(m, m.time, written))
    return made


async def step(chosen: Sequence[Member], targets: Sequence[VirtualDuration]) -> None:
    await asyncio.gather(
        *(
            one.machine.run(for_=one.start + t - one.machine.time)
            for one, t in zip(chosen, targets, strict=True)
        )
    )


async def drive(
    chosen: Sequence[Member], targets: Targets, seen: Callable[[int], bool] = lambda _: True
) -> list[list[Digest]]:
    """Run every member to each absolute target together, a digest of each at the targets
    `seen` keeps, by index."""
    digests: list[list[Digest]] = [[] for _ in chosen]
    for index, target in enumerate(targets):
        await step(chosen, [target] * len(chosen))
        if seen(index):
            for log, one in zip(digests, chosen, strict=True):
                log.append(digest(one.machine, one.restorable))
    return digests


async def in_turn(
    chosen: Sequence[Member], targets: Targets, seen: Callable[[int], bool] = lambda _: True
) -> list[list[Digest]]:
    """As `drive`, one member at a time in list order."""
    digests: list[list[Digest]] = [[] for _ in chosen]
    for index, target in enumerate(targets):
        for one in chosen:
            await step([one], [target])
        if seen(index):
            for log, one in zip(digests, chosen, strict=True):
                log.append(digest(one.machine, one.restorable))
    return digests


def frames(chosen: Sequence[Member]) -> list[tuple[tuple[int, int], ...]]:
    sent: list[tuple[tuple[int, int], ...]] = []
    for one in chosen:
        for pending in (one.written, one.late):
            if pending is not None:
                asyncio.run(pending)
        port = one.machine.periph.USART1.port
        got = asyncio.run(port.read_frames(port.available))
        sent.append(tuple((frame.data, (frame.at - one.start).ps) for frame in got))
    return sent


def every_ms(first: int, last: int) -> list[VirtualDuration]:
    return [ms(k) for k in range(first, last + 1)]


@dataclass(frozen=True)
class Reference:
    digests: list[list[Digest]]
    frames: list[tuple[tuple[int, int], ...]]


def late_line(one: Member) -> None:
    one.late = one.machine.periph.USART1.port.write(LATE_LINE, at=one.start + LATE_AT)


def reference(
    u575: popcorn.MachineDescription,
    steps: int = STEPS,
    hooked: bool = False,
    restorable: bool = False,
) -> Reference:
    """Each member run alone to 1, 2, ... `steps` ms after its boot point, its tick intercepted
    by a handler that only proceeds when `hooked`. With `restorable`, digests carry
    `Digest.restorable` for A and A gets a second line at `LATE_AT`."""
    alone = members(u575, ticks=[proceed] * len(LINES) if hooked else ())
    if restorable:
        alone[0].restorable = True
        late_line(alone[0])
    digests = [asyncio.run(drive([one], every_ms(1, steps)))[0] for one in alone]
    return Reference(digests, frames(alone))


def test_the_reference_echoes_its_line_and_diverges(u575: popcorn.MachineDescription) -> None:
    ref = reference(u575)
    a, b, c = ref.frames
    assert b"echo: alpha" in bytes(data for data, _ in a)
    assert b"echo: bravo charlie" in bytes(data for data, _ in b)
    assert c == ()
    assert ref.digests[0][-1] != ref.digests[1][-1]


def test_machines_driven_in_turn_equal_each_alone(u575: popcorn.MachineDescription) -> None:
    ref = reference(u575)
    chosen = members(u575)
    assert asyncio.run(in_turn(chosen, every_ms(1, STEPS))) == ref.digests
    assert frames(chosen) == ref.frames


def test_uneven_targets_in_reverse_order_equal_each_alone(
    u575: popcorn.MachineDescription,
) -> None:
    ref = reference(u575)
    chosen = members(u575)
    targets = [t for k in range(1, STEPS // 2 + 1) for t in (ms(2 * k - 2) + us(300), ms(2 * k))]
    got = asyncio.run(in_turn(chosen[::-1], targets, lambda index: index % 2 == 1))[::-1]
    assert got == [log[1::2] for log in ref.digests]
    assert frames(chosen) == ref.frames


@pytest.mark.parametrize("slice_instructions", [1_000_000, 1_000])
def test_machines_run_at_once_equal_each_alone(
    u575: popcorn.MachineDescription, slice_instructions: int
) -> None:
    ref = reference(u575)
    chosen = members(u575, slice_instructions)
    assert asyncio.run(drive(chosen, every_ms(1, STEPS))) == ref.digests
    assert frames(chosen) == ref.frames


def test_slices_of_1000_instructions_driven_in_turn_equal_each_alone(
    u575: popcorn.MachineDescription,
) -> None:
    ref = reference(u575)
    chosen = members(u575, 1_000)
    assert asyncio.run(in_turn(chosen, every_ms(1, STEPS))) == ref.digests
    assert frames(chosen) == ref.frames


class Rendezvous:
    """Holds each member's first tick of a step until every member reached its own, so the
    members are inside their runs at one wall instant, whatever the host load."""

    def __init__(self, parties: int) -> None:
        self.parties = parties
        self.barrier = threading.Barrier(parties, timeout=30)
        self.arrived = [False] * parties
        self.met = 0

    def arm(self) -> None:
        self.arrived = [False] * self.parties

    def handler(self, index: int) -> Handler:
        def tick(call: popcorn.Call) -> popcorn.Return:
            if not self.arrived[index]:
                self.arrived[index] = True
                if self.barrier.wait() == 0:
                    self.met += 1
            return call.proceed()

        return tick


def test_machines_held_inside_their_runs_together_equal_each_alone(
    u575: popcorn.MachineDescription,
) -> None:
    ref = reference(u575, hooked=True)
    meeting = Rendezvous(len(LINES))
    chosen = members(u575, 1_000, [meeting.handler(index) for index in range(len(LINES))])
    got: list[list[Digest]] = [[] for _ in chosen]

    async def body() -> None:
        for k in range(2, STEPS + 1, 2):
            meeting.arm()
            await step(chosen, [ms(k)] * len(chosen))
            for log, one in zip(got, chosen, strict=True):
                log.append(digest(one.machine, one.restorable))

    asyncio.run(body())
    assert meeting.met == STEPS // 2
    assert got == [log[1::2] for log in ref.digests]
    assert frames(chosen) == ref.frames


def test_a_restore_mid_run_leaves_the_other_machines_alone(
    u575: popcorn.MachineDescription,
) -> None:
    ref = reference(u575, 75, restorable=True)
    chosen = members(u575)
    chosen[0].restorable = True
    half = STEPS // 2
    first = asyncio.run(drive(chosen, every_ms(1, half)))
    assert first == [log[:half] for log in ref.digests]
    a = chosen[0].machine
    saved = a.snapshot()
    rest = asyncio.run(drive(chosen, every_ms(half + 1, STEPS)))
    before_late = LATE_AT.ps // ms(1).ps - 1
    assert rest[0][: before_late - half] == ref.digests[0][half:before_late]
    assert rest[1:] == [log[half:STEPS] for log in ref.digests[1:]]
    a.restore(saved)
    late_line(chosen[0])
    assert digest(a, restorable=True).restored() == ref.digests[0][half - 1].restored()
    for one, log in zip(chosen[1:], ref.digests[1:], strict=True):
        assert digest(one.machine) == log[STEPS - 1]

    async def again() -> list[list[Digest]]:
        digests: list[list[Digest]] = [[] for _ in chosen]
        for k in range(half + 1, STEPS + 1):
            await step(chosen, [ms(k), ms(k + half), ms(k + half)])
            for log, one in zip(digests, chosen, strict=True):
                log.append(digest(one.machine, one.restorable))
        return digests

    got = asyncio.run(again())
    assert [d.restored() for d in got[0]] == [d.restored() for d in ref.digests[0][half:STEPS]]
    since_restore = tuple(frame for frame in ref.frames[0] if frame[1] >= ms(half).ps)
    assert b"echo: delta" in bytes(data for data, _ in since_restore)
    assert frames(chosen[:1]) == [since_restore]
    assert got[1] == ref.digests[1][STEPS : STEPS + half]
    assert got[2] == ref.digests[2][STEPS : STEPS + half]
