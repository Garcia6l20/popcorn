"""One controller and two responders on one virtual clock, their UARTs linked.

The controller sends each responder its node id, then a 512-byte block every 16 ms and a 4-byte
poll every 100 ms. A responder answers a block with its checksum and a poll with its id. Both
directions run at 460800 baud, about 70 percent of the line.

A frame whose stop bit ends at the sender's system time t ends at the receiver at t + L, L the
link's latency. The receiver's USART raises IDLE one frame later. The results depend only on
the firmware and the latencies, never on the slicing, the quantum or the threads.
"""

import contextlib
import dataclasses
import itertools
import os
import threading
from collections.abc import AsyncGenerator, Callable, Generator, Sequence
from dataclasses import dataclass
from pathlib import Path

import popcorn
import pytest
from popcorn import Call, LineState, Machine, Return, StopReason, System, TapFrame, UartLink
from popcorn.units import VirtualDuration, VirtualTime, ms, s, us

RESPONDERS = ("responder1", "responder2")
TOWARDS = {"responder1": "controller.UART4", "responder2": "controller.USART3"}
LINK_ENDS = tuple(TOWARDS.values())
BLOCK_BYTES = 512
RING_BYTES = 256
POLL_BYTES = 4
REPLACE_AHEAD = 10_000
CYCLE_PS = 6250
WAKE_CYCLES = 4
LSI_HZ = 32_000
CUT = s(4)
CUT_AHEAD = ms(15)
REPOLL = ms(1500)

Frames = dict[str, tuple[TapFrame, ...]]
Idles = dict[str, tuple[tuple[VirtualTime, int], ...]]


@dataclass(frozen=True)
class Idle:
    """One IDLE interrupt of a responder's link: its local time, the ring position the DMA
    remaining count gives, and the thread the intercept ran on."""

    time: VirtualTime
    position: int
    thread: int


def block(seq: int) -> bytes:
    """The controller's block `seq`: a header, then a counter from `seq`."""
    return bytes([ord("B"), seq, *((seq + index) & 0xFF for index in range(2, BLOCK_BYTES))])


def checksum(data: bytes) -> int:
    """The firmware's `block_checksum`: the byte sum low, the sum of sums high, each 16 bits."""
    total = sums = 0
    for byte in data:
        total = (total + byte) & 0xFFFF
        sums = (sums + total) & 0xFFFF
    return sums << 16 | total


def messages(frames: Sequence[TapFrame]) -> list[list[TapFrame]]:
    """`frames` cut where the line went idle: the messages the receiver's IDLE separates."""
    cut: list[list[TapFrame]] = []
    for frame in frames:
        if cut and cut[-1][-1].end == frame.start:
            cut[-1].append(frame)
        else:
            cut.append([frame])
    return cut


def data(message: Sequence[TapFrame]) -> bytes:
    return bytes(frame.data for frame in message)


def record_idles(system: System) -> dict[str, list[Idle]]:
    """Log every IDLE interrupt of each responder's link, one list per member: the intercepts of
    two members run on two threads at once, so each appends to its own member's list."""
    log: dict[str, list[Idle]] = {}
    for name in RESPONDERS:
        member = system.members[name]
        seen = log.setdefault(name, [])

        def on_irq(call: Call, member: Machine = member, seen: list[Idle] = seen) -> Return:
            if member.periph.USART3.ISR_enabled.IDLE:
                remaining = member.periph.GPDMA1.GPDMA_C3BR1.BNDT
                seen.append(Idle(member.time, RING_BYTES - remaining, threading.get_ident()))
            return call.proceed()

        member.intercept("USART3_IRQHandler", on_irq)
    return log


def tapped(system: System) -> Frames:
    """Every frame the controller's two links carried since the last read, by receiving end."""
    taken: Frames = {}
    for end in LINK_ENDS:
        tap = system.tap(end)
        for towards in tap.ends:
            taken[towards] = tuple(tap.frames(towards=towards))
    return taken


@contextlib.asynccontextmanager
async def booted(
    spec: popcorn.SystemDescription, slice_instructions: int = 1_000_000
) -> AsyncGenerator[System]:
    """`spec` booted member by member and linked, as a script would. After, the system is
    closed, then each member's board."""
    machines = spec.machines()
    try:
        for name, machine in machines.items():
            await spec.members[name].boot_machine(machine)
        system = spec.link(machines, slice_instructions=slice_instructions)
        try:
            yield system
        finally:
            system.close()
    finally:
        for machine in machines.values():
            machine.board.close()


async def run_for(system: System, duration: VirtualDuration) -> None:
    stop = await system.run(for_=duration)
    assert stop.reason is StopReason.TIME, stop


async def one_second(system: System) -> tuple[Frames, Idles]:
    """Run `system` for 1 s: the frames on both links and each responder's IDLE log."""
    idles = record_idles(system)
    await run_for(system, s(1))
    return tapped(system), {
        name: tuple((idle.time, idle.position) for idle in log) for name, log in idles.items()
    }


async def test_each_responder_gets_its_node_id(system: System) -> None:
    await run_for(system, ms(1))
    assert system.members["responder1"].var("node_id").value == 1
    assert system.members["responder2"].var("node_id").value == 2


async def test_every_block_comes_back_with_its_checksum(system: System) -> None:
    await run_for(system, s(1))
    controller = system.members["controller"]
    frames = tapped(system)
    for index, name in enumerate(RESPONDERS):
        state = controller.var("responders")[index]
        sent = [data(m) for m in messages(frames[f"{name}.USART3"])]
        answers = [data(m) for m in messages(frames[TOWARDS[name]])]
        blocks = [m for m in sent if len(m) == BLOCK_BYTES]
        sums = [a for a in answers if a[0] == ord("b")]
        assert sent[0] == bytes([ord("N"), index + 1])
        assert answers[0] == bytes([ord("n"), index + 1])
        assert blocks == [block(seq) for seq in range(len(blocks))]
        assert len(blocks) >= 60
        assert [a[:3] for a in sums] == [
            bytes([ord("b"), index + 1, seq]) for seq in range(len(sums))
        ]
        assert [int.from_bytes(a[3:7], "little") for a in sums] == [
            checksum(b) for b in blocks[: len(sums)]
        ]
        assert state.sums_ok.value == len(sums) == system.members[name].var("blocks_answered").value
        assert state.sums_bad.value == 0


async def test_idle_is_one_frame_after_the_last_byte_arrives(
    system: System, system_description: popcorn.SystemDescription
) -> None:
    latency = {link.b.partition(".")[0]: link.latency for link in system_description.links}
    idles = record_idles(system)
    await run_for(system, s(1))
    frames = tapped(system)
    for name in RESPONDERS:
        own = frames[TOWARDS[name]]
        frame_ps = own[0].end.ps - own[0].start.ps
        assert all(f.end.ps - f.start.ps == frame_ps for f in own)
        expected: list[tuple[int, int]] = []
        count = 0
        for message in messages(frames[f"{name}.USART3"]):
            count += len(message)
            flag = system.to_local(
                name, VirtualTime(message[-1].end.ps + latency[name].ps + frame_ps)
            )
            woken = -(-flag.ps // CYCLE_PS) * CYCLE_PS + WAKE_CYCLES * CYCLE_PS
            if woken <= system.members[name].time.ps:
                expected.append((woken, count % RING_BYTES))
        assert len(expected) >= 70
        assert [(idle.time.ps, idle.position) for idle in idles[name]] == expected


async def test_member_callbacks_run_on_their_own_threads(system: System) -> None:
    idles = record_idles(system)
    await run_for(system, ms(100))
    threads = {name: {idle.thread for idle in log} for name, log in idles.items()}
    assert all(len(seen) == 1 for seen in threads.values())
    first, second = (seen.pop() for seen in threads.values())
    assert first != second
    assert threading.get_ident() not in (first, second)


async def node_id_exchange(
    spec: popcorn.SystemDescription,
) -> tuple[dict[str, VirtualTime], VirtualTime]:
    """Each responder's first IDLE, at its node id, in system time, and when the controller has
    responder 1's answer in full."""
    async with booted(spec) as system:
        idles = record_idles(system)
        await run_for(system, ms(5))
        first = {name: system.to_system(name, log[0].time) for name, log in idles.items()}
        answer = messages(system.tap("controller.UART4").frames(towards="controller.UART4"))[0]
        return first, answer[-1].arrival


async def test_latency_is_a_model_parameter(system_description: popcorn.SystemDescription) -> None:
    slower = [
        dataclasses.replace(system_description.links[0], latency=us(50)),
        system_description.links[1],
    ]
    fast, fast_answer = await node_id_exchange(system_description)
    slow, slow_answer = await node_id_exchange(system_description.replace(links=slower))
    assert slow["responder1"].ps - fast["responder1"].ps == us(30).ps
    assert slow["responder2"] == fast["responder2"]
    assert slow_answer.ps - fast_answer.ps == us(60).ps


@contextlib.contextmanager
def pinned_to_one_cpu() -> Generator[int]:
    """Pin every thread of this process, and so every thread it starts, to one CPU."""
    before = {tid: os.sched_getaffinity(tid) for tid in _threads()}
    cpu = min(os.sched_getaffinity(0))
    for tid in before:
        os.sched_setaffinity(tid, {cpu})
    try:
        yield cpu
    finally:
        for tid, mask in before.items():
            with contextlib.suppress(OSError):
                os.sched_setaffinity(tid, mask)


def _threads() -> list[int]:
    return [int(tid) for tid in os.listdir("/proc/self/task")]


def _third_link(spec: popcorn.SystemDescription) -> popcorn.SystemDescription:
    """`spec` with a 3 us link between two unused LPUART1s, which lowers the quantum to 3 us."""
    third = UartLink("controller.LPUART1", "responder1.LPUART1", latency=us(3), baud=460_800)
    return spec.replace(links=[*spec.links, third])


PINNABLE = hasattr(os, "sched_setaffinity") and Path("/proc/self/task").is_dir()


@pytest.mark.parametrize("variant", ["slicing", "quantum", "one cpu"])
async def test_the_run_is_the_same_at_any_slicing_quantum_or_cpu_count(
    system_description: popcorn.SystemDescription, variant: str
) -> None:
    if variant == "one cpu" and not PINNABLE:
        pytest.skip("pinning threads needs Linux")
    async with booted(system_description) as system:
        reference = await one_second(system)
    if variant == "slicing":
        async with booted(system_description, slice_instructions=997) as system:
            assert await one_second(system) == reference
    elif variant == "quantum":
        spec = _third_link(system_description)
        assert spec.quantum == us(3)
        async with booted(spec) as system:
            assert await one_second(system) == reference
    else:
        with pinned_to_one_cpu() as cpu:
            before = set(_threads())
            async with booted(system_description) as system:
                observed = await one_second(system)
                started = set(_threads()) - before
                assert len(started) >= len(system_description.members)
                assert all(os.sched_getaffinity(tid) == {cpu} for tid in started)
        assert observed == reference


async def test_a_starved_responder_resets_and_comes_back(system: System) -> None:
    controller = system.members["controller"]
    responder = system.members["responder2"]
    refreshes: list[VirtualTime] = []
    events: list[tuple[str, VirtualTime]] = []

    def refreshed(call: Call) -> Return:
        refreshes.append(system.to_system(responder, responder.time))
        return call.proceed()

    def reported(what: str) -> Callable[[Call], Return]:
        def report(call: Call) -> Return:
            if call.arg(0) == 2:
                events.append((what, system.to_system(controller, controller.time)))
            return call.proceed()

        return report

    responder.intercept("HAL_IWDG_Refresh", refreshed)
    controller.intercept("responder_lost", reported("lost"))
    controller.intercept("responder_found", reported("found"))
    await system.run(until=VirtualTime(ms(1005).ps))
    last_refresh = refreshes[-1].ps
    period_ps = (4 << responder.periph.IWDG.PR.value) * 10**12 // LSI_HZ
    bite = last_refresh + responder.periph.IWDG.RLR.value * period_ps
    responder.var("starve").value = 1
    events.clear()

    await run_for(system, s(4))
    assert system.to_system("responder2", VirtualTime(0)).ps == bite
    answers = system.tap("controller.USART3").frames(towards="controller.USART3")
    lost = [frame for frame in answers if frame.lost]
    assert [frame.end.ps for frame in lost] == [bite]
    last = [m for m in messages(answers) if m[-1].end < lost[0].start][-1][-1]
    answered = last.arrival.ps + last.end.ps - last.start.ps
    assert [what for what, _ in events] == ["lost", "found"]
    (_, lost_at), (_, found_at) = events
    assert s(3).ps - ms(1).ps < lost_at.ps - answered <= s(3).ps
    assert lost_at < found_at < VirtualTime(lost_at.ps + ms(10).ps)
    assert b"responder: up, watchdog reset\r\n" in responder.periph.USART1.port.take()
    shown = controller.periph.USART1.port.take()
    assert b"controller: responder 2 lost\r\ncontroller: responder 2 back\r\n" in shown

    starts = [
        m[0].start.ps
        for m in messages(system.tap("controller.UART4").frames(towards="responder1.USART3"))
        if len(m) == BLOCK_BYTES
    ]
    assert max(b - a for a, b in itertools.pairwise(starts)) < ms(20).ps
    assert controller.var("responders")[0].sums_bad.value == 0


@pytest.mark.parametrize("line", ["idle", "break"])
async def test_a_pulled_cable_loses_the_responder_and_it_rejoins(
    system: System, line: LineState
) -> None:
    controller = system.members["controller"]
    responder = system.members["responder1"]
    tap = system.tap("controller.UART4")
    events: list[tuple[str, VirtualTime]] = []

    def reported(what: str) -> Callable[[Call], Return]:
        def report(call: Call) -> Return:
            if call.arg(0) == 1:
                events.append((what, system.to_system(controller, controller.time)))
            return call.proceed()

        return report

    controller.intercept("responder_lost", reported("lost"))
    controller.intercept("responder_found", reported("found"))
    await run_for(system, ms(500))
    events.clear()
    responder.periph.USART1.port.take()
    controller.periph.USART1.port.take()
    now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
    start = VirtualTime(now.ps + CUT_AHEAD.ps)
    end = VirtualTime(start.ps + CUT.ps)
    tap.cut(start, end, line=line)

    await run_for(system, s(6))
    assert [what for what, _ in events] == ["lost", "found"]
    (_, lost_at), (_, found_at) = events
    answers = tap.frames(towards="controller.UART4")
    last = [m for m in messages(answers) if m[-1].end <= start][-1][-1]
    answered = last.arrival.ps + last.end.ps - last.start.ps
    assert s(3).ps - ms(1).ps < lost_at.ps - answered <= s(3).ps
    assert end < found_at
    assert REPOLL.ps < found_at.ps - lost_at.ps < REPOLL.ps + ms(5).ps

    sent = tap.frames(towards="responder1.USART3")
    for frames in (answers, sent):
        assert any(f.cut and start <= f.start for f in frames)
        assert any(f.lost for f in frames)
        for frame in frames:
            crossing = frame.start < start < frame.end or frame.start < end < frame.end
            assert frame.lost == crossing
            assert frame.cut == (crossing or start <= frame.start < end)

    shown = controller.periph.USART1.port.take()
    assert shown == b"controller: responder 1 lost\r\ncontroller: responder 1 back\r\n"
    assert responder.periph.USART1.port.take() == b"responder: node id 1\r\n"
    errors = 1 if line == "break" else 0
    assert controller.var("responders")[0].link.errors.value == errors
    assert responder.var("uplink").errors.value == errors
    for index in range(len(RESPONDERS)):
        state = controller.var("responders")[index]
        assert state.sums_bad.value == 0
        assert state.times_lost.value == (1 if index == 0 else 0)
    after = [m for m in messages(sent) if m[0].start > found_at and len(m) == BLOCK_BYTES]
    assert len(after) >= 30


async def test_a_replaced_frame_changes_the_checksum_of_its_block(system: System) -> None:
    controller = system.members["controller"]
    tap = system.tap("controller.UART4")
    await run_for(system, ms(500))
    index = tap.posted(towards="responder1.USART3") + REPLACE_AHEAD
    async with system.running():
        tap.replace(index, b"\x00", towards="responder1.USART3")
        await controller.elapse(ms(400))
    line = messages(tap.frames(towards="responder1.USART3"))
    holding = next(m for m in line if m[0].index <= index <= m[-1].index)
    offset = index - holding[0].index
    seq = data(holding)[1]
    assert len(holding) == BLOCK_BYTES
    assert holding[offset].data == 0 != block(seq)[offset]
    changed = [m for m in line if len(m) == BLOCK_BYTES and data(m) != block(data(m)[1])]
    assert changed == [holding]
    state = controller.var("responders")[0]
    assert state.sums_bad.value == 1
    assert state.last_bad_seq.value == seq


async def test_an_injected_poll_is_answered(system: System) -> None:
    await run_for(system, s(2))
    tap = system.tap("controller.USART3")
    line = tap.frames(towards="responder2.USART3")
    for _ in range(400):
        cut = messages(line)
        since_block = system.time.ps - cut[-1][-1].end.ps
        since_poll = system.time.ps - [m for m in cut if len(m) == POLL_BYTES][-1][-1].end.ps
        if len(cut[-1]) == BLOCK_BYTES and since_block < us(500).ps and since_poll < ms(50).ps:
            break
        await run_for(system, us(250))
        line += tap.frames(towards="responder2.USART3")
    else:
        pytest.fail("no quiet gap on responder 2's line within 100 ms")
    at = VirtualTime(system.time.ps + us(100).ps)
    tap.inject(b"P\x7f\x00\x00", at=at, towards="responder2.USART3")

    await run_for(system, ms(20))
    injected = [f for f in tap.frames(towards="responder2.USART3") if f.injected]
    assert [f.data for f in injected] == list(b"P\x7f\x00\x00")
    assert injected[0].start == at
    assert all(f.arrival == f.end for f in injected)
    answers = [data(m) for m in messages(tap.frames(towards="controller.USART3"))]
    assert b"p\x02\x7f\x00" in answers


async def test_a_restored_snapshot_replays_the_same_frames(system: System) -> None:
    await run_for(system, ms(500))
    tapped(system)
    saved = system.snapshot()
    await run_for(system, ms(500))
    first = tapped(system)
    system.restore(saved)
    await run_for(system, ms(500))
    assert tapped(system) == first
