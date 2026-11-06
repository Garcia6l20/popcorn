"""The virtual-time asyncio loop of a machine: timers as machine events at logical stamps, device
hooks inside its steps, errors, resets and close, and the same bytes and stamps at any block shape.
"""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import gc
import inspect
import logging
import threading
import weakref
from collections.abc import AsyncIterator, Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import VirtualLoop, VirtualLoopUnsupported, _core
from popcorn.arm import Reg
from popcorn.board import UartDevice, UartError
from popcorn.units import VirtualTime, ms, ps, us

from .conftest import BOARD_ELF, collected

WFI_LOOP = b"\x30\xbf\xfd\xe7"
BRANCH_TO_SELF = b"\xfe\xe7"
NOP = b"\x00\xbf"
UE_CLEAR_AND_SET = bytes.fromhex("0348" "0021" "0160" "0349" "0160" "30bf" "fde7" "00bf")
UE_TE_RE = (0xD).to_bytes(4, "little")
RAM = 0x2000_0000
THUMB = 1 << 24
BRR = 35
PS_PER_SECOND = 10**12
STEP_PS = 250_000
FRAME_PS = 87_500_000


def nop_loop(nops: int) -> bytes:
    branch = 0xE000 | ((-(2 * nops + 4)) >> 1 & 0x7FF)
    return NOP * nops + branch.to_bytes(2, "little")


def stamp(loop: asyncio.AbstractEventLoop) -> int:
    return round(loop.time() * PS_PER_SECOND)


def parked_on(m: popcorn.Machine, code: bytes = WFI_LOOP) -> None:
    m.mem.write(RAM, code)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1


def uart_on(m: popcorn.Machine, *, rtse: bool = False) -> None:
    m.periph.RCC.APB2ENR.USART1EN = 1
    m.periph.USART1.BRR.value = BRR
    if rtse:
        m.periph.USART1.CR3.RTSE = 1
    m.periph.USART1.CR1_disabled.poke(UE=1, TE=1, RE=1)


def asleep(
    u575: popcorn.PartDescription,
    devices: Mapping[str, Sequence[UartDevice]] | None = None,
    code: bytes = WFI_LOOP,
    *,
    rtse: bool = False,
) -> popcorn.Machine:
    m = popcorn.Machine(u575, devices=devices)
    m.reset()
    parked_on(m, code)
    uart_on(m, rtse=rtse)
    return m


def run(m: popcorn.Machine, duration: popcorn.units.VirtualDuration) -> None:
    asyncio.run(m.run(for_=duration))


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


class Stack(UartDevice):
    """Runs `build` inside `on_attach` and logs what it hears with `now`."""

    def __init__(self, build: Callable[[Stack], None] | None = None) -> None:
        super().__init__()
        self.build = build
        self.log: list[tuple[str, int, int]] = []
        self.sent: list[int] = []

    def on_attach(self) -> None:
        if self.build is not None:
            self.build(self)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.log.append((f"frame {byte:#x}", at.ps, self.now.ps))

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.log.append((kind.name, at.ps, self.now.ps))

    def mark(self, what: str) -> None:
        self.log.append((what, stamp(self.loop), self.now.ps))


def scenario(build: Callable[[Stack], None]) -> Callable[[popcorn.PartDescription], Stack]:
    def ran(u575: popcorn.PartDescription) -> Stack:
        device = Stack(build)
        m = asleep(u575, {"USART1": [device]})
        run(m, ms(20))
        return device

    return ran


def test_the_loop_is_one_per_machine_and_its_devices_share_it(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    other = asleep(u575)
    assert isinstance(m.loop, VirtualLoop)
    assert m.loop is device.loop is m.loop
    assert other.loop is not m.loop


def test_a_stack_is_built_inside_a_step_and_a_task_starts_at_that_stamp(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[object] = []

    async def body(device: Stack) -> None:
        seen.append(asyncio.current_task() is not None)
        device.mark("task")

    def build(device: Stack) -> None:
        seen.append(asyncio.get_running_loop() is device.loop)
        seen.append(_core.on_host_thread())
        asyncio.get_running_loop().create_task(body(device))

    device = scenario(build)(u575)
    assert seen == [True, True, True]
    assert device.log == [("task", 0, 0)]


def test_the_asyncio_surface_runs_at_exact_stamps(u575: popcorn.PartDescription) -> None:
    out: list[tuple[str, int]] = []

    def noted(what: str) -> None:
        out.append((what, stamp(asyncio.get_running_loop())))

    async def primitives() -> None:
        event = asyncio.Event()
        queue: asyncio.Queue[int] = asyncio.Queue()
        lock = asyncio.Lock()

        async def setter() -> None:
            await asyncio.sleep(0.001)
            event.set()
            queue.put_nowait(7)

        task = asyncio.create_task(setter())
        await event.wait()
        noted("event")
        assert await queue.get() == 7
        noted("queue")
        async with lock:
            await asyncio.sleep(0.0005)
        noted("lock")
        await task

    async def timers() -> None:
        await asyncio.sleep(0)
        noted("sleep 0")
        await asyncio.sleep(0.002)
        noted("sleep")
        assert await asyncio.wait_for(asyncio.sleep(0.001, "x"), 0.002) == "x"
        noted("wait_for in time")
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(asyncio.sleep(1), 0.0015)
        noted("wait_for timeout")
        await asyncio.gather(asyncio.sleep(0.001), asyncio.sleep(0.002))
        noted("gather")
        async with asyncio.TaskGroup() as group:
            group.create_task(asyncio.sleep(0.001))
            group.create_task(asyncio.sleep(0.0005))
        noted("task group")
        with pytest.raises(TimeoutError):
            async with asyncio.timeout(0.0001):
                await asyncio.sleep(1)
        noted("timeout")
        inner = asyncio.create_task(asyncio.sleep(0.0002, "shielded"))
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(asyncio.shield(inner), 0.0001)
        assert await inner == "shielded"
        noted("shield")
        loser = asyncio.create_task(asyncio.sleep(1))
        await asyncio.sleep(0.0001)
        loser.cancel()
        with pytest.raises(asyncio.CancelledError):
            await loser
        noted("cancel")
        loop = asyncio.get_running_loop()
        done: asyncio.Future[str] = loop.create_future()
        loop.call_at(loop.time() - 1, done.set_result, "past")
        assert await done == "past"
        noted("call_at past")
        await primitives()

    def build(device: Stack) -> None:
        asyncio.get_running_loop().create_task(timers())

    scenario(build)(u575)
    assert out == [
        ("sleep 0", 0),
        ("sleep", us(2000).ps),
        ("wait_for in time", us(3000).ps),
        ("wait_for timeout", us(4500).ps),
        ("gather", us(6500).ps),
        ("task group", us(7500).ps),
        ("timeout", us(7600).ps),
        ("shield", us(7800).ps),
        ("cancel", us(7900).ps),
        ("call_at past", us(7900).ps),
        ("event", us(8900).ps),
        ("queue", us(8900).ps),
        ("lock", us(9400).ps),
    ]


def test_callbacks_run_first_in_first_out_and_equal_due_times_in_call_order(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []

    def build(device: Stack) -> None:
        loop = asyncio.get_running_loop()
        for name in "abc":
            loop.call_soon(out.append, name)
        for name in "xyz":
            loop.call_later(0.001, out.append, name)

    scenario(build)(u575)
    assert out == ["a", "b", "c", "x", "y", "z"]


def fire_times(u575: popcorn.PartDescription, code: bytes) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []

    def build(device: Stack) -> None:
        loop = device.loop

        def tick(left: int) -> None:
            machine = machine_of[0]
            out.append((stamp(loop), machine.time.ps))
            if left:
                loop.call_later(0.0001, tick, left - 1)

        loop.call_later(0.0001, tick, 7)

    device = Stack(build)
    m = asleep(u575, {"USART1": [device]}, code)
    machine_of = [m]
    run(m, ms(2))
    return out


def test_a_timer_fires_at_its_logical_stamp_and_within_a_block_on_the_clock(
    u575: popcorn.PartDescription,
) -> None:
    short = fire_times(u575, nop_loop(2))
    long = fire_times(u575, nop_loop(40))
    expected = [us(100 * (n + 1)).ps for n in range(8)]
    assert [logical for logical, _ in short] == expected
    assert [logical for logical, _ in long] == expected
    assert any(real > logical for logical, real in long)
    for logical, real in short:
        assert logical <= real <= logical + 4 * STEP_PS
    for logical, real in long:
        assert logical <= real <= logical + 42 * STEP_PS


def test_a_timer_due_at_time_zero_dispatches_before_the_first_block(
    u575: popcorn.PartDescription,
) -> None:
    out: list[tuple[int, int]] = []
    m = popcorn.Machine(u575)
    m.reset()
    parked_on(m, BRANCH_TO_SELF)
    uart_on(m)
    assert m.time.ps == 0

    def fired() -> None:
        out.append((stamp(m.loop), m.time.ps))

    m.loop.call_at(0.0, fired)
    run(m, us(10))
    assert out == [(0, 0)]


def test_delays_round_to_the_nearest_picosecond_and_a_positive_one_is_at_least_one(
    u575: popcorn.PartDescription,
) -> None:
    out: dict[str, int] = {}

    def build(device: Stack) -> None:
        loop = device.loop
        for name, delay in (("10 ms", 0.01), ("1 us", 1e-6), ("tiny", 1e-15), ("zero", 0)):
            loop.call_later(delay, lambda n=name: out.__setitem__(n, stamp(loop)))

    scenario(build)(u575)
    assert out == {"zero": 0, "tiny": 1, "1 us": 1_000_000, "10 ms": 10_000_000_000}


@pytest.mark.parametrize("delay", [1e10, float("inf")])
def test_a_due_time_past_the_clock_range_stays_unarmed(
    u575: popcorn.PartDescription, delay: float
) -> None:
    m = asleep(u575)
    fired: list[int] = []
    m.loop.call_later(delay, fired.append, 1)
    asyncio.run(m.run())
    assert fired == []
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.IDLE


def test_a_call_with_a_bad_argument_is_refused_and_leaves_nothing(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575)
    bad: Any = "soon"
    with pytest.raises(TypeError, match="number of seconds"):
        m.loop.call_later(bad, print)
    with pytest.raises(TypeError, match="number of seconds"):
        m.loop.call_at(bad, print)
    with pytest.raises(TypeError, match="callable"):
        m.loop.call_soon(bad)
    with pytest.raises(TypeError, match="callable"):
        m.loop.call_later(1, bad)
    with pytest.raises(ValueError):
        m.loop.call_later(float("nan"), print)
    with pytest.raises(TypeError, match="callable or None"):
        m.loop.set_exception_handler(bad)
    with pytest.raises(TypeError, match="callable or None"):
        m.loop.set_task_factory(bad)
    asyncio.run(m.run())
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.IDLE


@pytest.mark.parametrize(
    "kind", [popcorn.ResetKind.SOFTWARE, popcorn.ResetKind.PIN, popcorn.ResetKind.POWER_ON]
)
def test_a_timer_keeps_its_instant_through_a_reset(
    u575: popcorn.PartDescription, kind: popcorn.ResetKind
) -> None:
    out: list[tuple[int, int]] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, ms(1))
    m.reset(popcorn.ResetKind.SOFTWARE)
    parked_on(m)
    uart_on(m)
    run(m, ms(1))
    board = stamp(device.loop)
    device.loop.call_later(0.010, lambda: out.append((stamp(device.loop), device.now.ps)))
    run(m, ms(2))
    before = stamp(device.loop)
    assert before >= board + ms(2).ps
    m.reset(kind)
    assert stamp(device.loop) >= before
    assert m.time.ps == 0
    parked_on(m)
    uart_on(m)
    run(m, ms(9))
    due = board + ms(10).ps
    assert out[0][0] == due
    base = before
    assert out[0][1] == due - base
    assert len(out) == 1


def test_a_timer_due_before_a_reset_fires_after_it_at_the_reset_stamp(
    u575: popcorn.PartDescription,
) -> None:
    out: list[tuple[int, int]] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]}, nop_loop(40))
    run(m, ms(1))
    device.loop.call_later(0.0, lambda: out.append((stamp(device.loop), device.now.ps)))
    m.reset(popcorn.ResetKind.SOFTWARE)
    reset_at = stamp(device.loop)
    parked_on(m)
    uart_on(m)
    run(m, us(10))
    assert out == [(reset_at, 0)]
    assert reset_at >= ms(1).ps


def test_a_reset_hook_runs_in_the_loop_on_the_caller_and_restores_the_real_loop(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[object] = []
    late: list[int] = []

    async def first_step(device: Stack) -> None:
        seen.append(asyncio.get_running_loop() is device.loop)
        seen.append(asyncio.current_task() is not None)
        await asyncio.sleep(0)
        late.append(device.now.ps)

    class Resetting(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            loop = asyncio.get_running_loop()
            seen.append(loop is self.loop)
            seen.append(threading.current_thread() is threading.main_thread())
            with pytest.raises(RuntimeError, match="inside reset"):
                self.send(b"\x41")
            asyncio.Task(first_step(self), loop=loop, eager_start=True)

    device = Resetting()
    m = popcorn.Machine(u575, devices={"USART1": [device]})

    async def inside_a_real_loop() -> None:
        real = asyncio.get_running_loop()
        me = asyncio.current_task()
        m.reset()
        assert asyncio.get_running_loop() is real
        assert asyncio.current_task() is me

    asyncio.run(inside_a_real_loop())
    assert seen == [True, True, True, True]
    assert late == []
    parked_on(m)
    uart_on(m)
    run(m, us(1))
    assert late == [0]


def test_a_raising_reset_hook_restores_the_real_loop(u575: popcorn.PartDescription) -> None:
    class Raising(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            raise LookupError("reset says no")

    m = popcorn.Machine(u575, devices={"USART1": [Raising()]})

    async def inside_a_real_loop() -> None:
        real = asyncio.get_running_loop()
        me = asyncio.current_task()
        with pytest.raises(popcorn.ModelError, match="reset says no"):
            m.reset()
        assert asyncio.get_running_loop() is real
        assert asyncio.current_task() is me

    asyncio.run(inside_a_real_loop())


def test_a_base_exception_from_a_reset_hook_restores_the_real_loop(
    u575: popcorn.PartDescription,
) -> None:
    class Stopping(BaseException):
        pass

    class Raising(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            raise Stopping

    m = popcorn.Machine(u575, devices={"USART1": [Raising()]})

    async def inside_a_real_loop() -> None:
        real = asyncio.get_running_loop()
        me = asyncio.current_task()
        with pytest.raises(Stopping):
            m.reset()
        assert asyncio.get_running_loop() is real
        assert asyncio.current_task() is me

    asyncio.run(inside_a_real_loop())


def test_machine_access_from_a_callback_raises_machine_running(
    u575: popcorn.PartDescription,
) -> None:
    caught: list[BaseException] = []
    m = asleep(u575)

    def touches() -> None:
        try:
            m.mem.u32[RAM]
        except popcorn.MachineRunning as error:
            caught.append(error)

    m.loop.call_later(0.0001, touches)
    run(m, ms(1))
    assert len(caught) == 1


def test_a_parked_machine_takes_calls_from_the_test_and_a_running_one_refuses(
    u575: popcorn.PartDescription,
) -> None:
    out: list[tuple[str, bool]] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    loop = device.loop

    def noted(what: str) -> None:
        out.append((what, _core.on_host_thread()))

    loop.call_soon(noted, "soon")
    loop.call_later(0.0001, noted, "later")
    loop.call_at(loop.time() + 0.0002, noted, "at")
    cancelled = loop.call_later(0.0001, noted, "cancelled")
    cancelled.cancel()

    async def sits() -> None:
        noted("task")

    loop.create_task(sits())
    assert out == []
    assert not loop.is_running()
    run(m, ms(1))
    assert out == [
        ("soon", True),
        ("task", True),
        ("later", True),
        ("at", True),
    ]

    async def while_running() -> None:
        async with m.running():
            for call in (
                lambda: loop.call_soon(noted, "no"),
                lambda: loop.call_later(1, noted, "no"),
                lambda: loop.call_at(1, noted, "no"),
                loop.time,
                loop.close,
            ):
                with pytest.raises(popcorn.MachineRunning):
                    call()
            coroutine = sits()
            with pytest.raises(popcorn.MachineRunning):
                loop.create_task(coroutine)
            coroutine.close()
            with pytest.raises(popcorn.MachineRunning):
                kept.cancel()
            assert not kept.cancelled()

    kept = loop.call_later(1.0, noted, "kept")
    asyncio.run(while_running())
    assert not loop.is_closed()
    run(m, ms(1100))
    assert out[4:] == [("kept", True)]


def test_a_parked_cancel_withdraws_the_event_so_a_bare_run_stops_idle(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575)
    handle = m.loop.call_later(3600, print)
    handle.cancel()
    start = m.time
    asyncio.run(m.run())
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.IDLE
    assert m.time.ps < start.ps + ms(1).ps


def test_a_test_thread_reading_during_a_hook_is_not_taken_for_the_hook(
    u575: popcorn.PartDescription,
) -> None:
    entered = threading.Event()
    leave = threading.Event()

    class Blocking(Stack):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            entered.set()
            assert leave.wait(10)

    device = Blocking()
    m = asleep(u575, {"USART1": [device]})

    async def body() -> None:
        m.periph.USART1.TDR.value = 0x41
        async with m.running():
            await asyncio.to_thread(entered.wait, 10)
            try:
                with pytest.raises(popcorn.MachineRunning):
                    _ = device.now
                with pytest.raises(RuntimeError, match="runs on the machine's thread"):
                    device.send(b"\x41")
                with pytest.raises(popcorn.MachineRunning):
                    device.loop.time()
                with pytest.raises(popcorn.MachineRunning):
                    device.loop.close()
                assert not device.loop.is_closed()
                assert not device.loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
            finally:
                leave.set()

    asyncio.run(body())
    assert not device.loop.is_closed()


def test_gil_entries_count_each_byte_and_each_stamp_a_timer_fires_and_none_idle(
    u575: popcorn.PartDescription,
) -> None:
    class Replying(Stack):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            super().on_frame(byte, at)
            self.loop.call_later(0.00001, lambda: None)

    device = Replying()
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    device.loop.call_soon(device.send, b"abcdefgh")
    run(m, us(1))
    before = gil_entries(m)
    run(m, ms(20))
    assert len(device.log) == 8
    assert gil_entries(m) - before == 16

    idle = asleep(u575)
    ticks: list[int] = []

    def tick(left: int) -> None:
        ticks.append(left)
        if left:
            idle.loop.call_later(0.001, tick, left - 1)

    idle.loop.call_later(0.001, tick, 9)
    run(idle, us(500))
    before = gil_entries(idle)
    run(idle, ms(20))
    assert len(ticks) == 10
    assert gil_entries(idle) - before == 10
    before = gil_entries(idle)
    run(idle, ms(20))
    assert gil_entries(idle) == before


def test_an_armed_timer_is_reached_by_the_idle_jump_with_one_gil_entry(
    u575: popcorn.PartDescription,
) -> None:
    fired: list[int] = []
    m = asleep(u575)
    m.loop.call_later(3600, lambda: fired.append(stamp(m.loop)))
    before = gil_entries(m)
    run(m, ms(3601000))
    assert fired == [3600 * PS_PER_SECOND]
    assert gil_entries(m) - before == 1


def test_a_timer_past_a_run_does_not_fire_and_waits_for_the_next_run(
    u575: popcorn.PartDescription,
) -> None:
    fired: list[int] = []
    m = asleep(u575)
    m.loop.call_later(0.002, lambda: fired.append(stamp(m.loop)))
    run(m, ms(1))
    assert fired == []
    run(m, ms(2))
    assert fired == [ms(2).ps]


def test_a_repeating_timer_counts_as_due_until_it_stops(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)
    ticks: list[int] = []

    def tick() -> None:
        ticks.append(stamp(m.loop))
        if len(ticks) < 10:
            m.loop.call_later(0.001, tick)

    m.loop.call_later(0.001, tick)
    asyncio.run(m.run())
    assert ticks == [ms(n + 1).ps for n in range(10)]
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.IDLE
    assert m.time.ps >= ms(10).ps


def test_a_never_quiet_chain_raises_at_the_bound_and_the_machine_goes_on(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575)
    runs = [0]

    def chain() -> None:
        runs[0] += 1
        if runs[0] < 70000:
            m.loop.call_soon(chain)

    m.loop.call_soon(chain)
    with pytest.raises(popcorn.ModelError, match="65536 callbacks at one stamp"):
        run(m, ms(1))
    assert runs[0] == 65536
    run(m, ms(1))
    assert runs[0] == 70000
    assert not m.loop.is_closed()


def test_a_chain_below_one_instruction_stops_with_spin(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)

    def chain() -> None:
        m.loop.call_later(1e-12, chain)

    m.loop.call_soon(chain)
    run(m, ms(1))
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.SPIN
    assert "loop" in str(m.last_stop)


def test_a_raising_callback_finishes_the_step_and_raises_from_the_goal(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []
    m = asleep(u575)

    def raises() -> None:
        raise LookupError("no such entry")

    m.loop.call_later(0.001, raises)
    m.loop.call_later(0.001, out.append, "same stamp")
    m.loop.call_later(0.002, out.append, "next stamp")
    with pytest.raises(popcorn.ModelError, match="no such entry") as caught:
        run(m, ms(5))
    assert isinstance(caught.value.__cause__, LookupError)
    assert out == ["same stamp"]
    run(m, ms(5))
    assert out == ["same stamp", "next stamp"]


def test_a_task_failing_and_awaited_in_its_step_raises_nothing(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []
    m = asleep(u575)

    async def fails() -> None:
        raise LookupError("expected")

    async def awaiter() -> None:
        try:
            await m.loop.create_task(fails())
        except LookupError:
            out.append("caught")

    m.loop.create_task(awaiter())
    run(m, ms(1))
    assert out == ["caught"]


async def fails_soon() -> None:
    await asyncio.sleep(0.001)
    raise LookupError("task says no")


def test_a_task_failure_nothing_awaited_raises_and_names_the_task(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575)
    m.loop.create_task(fails_soon(), name="the stack")
    with pytest.raises(popcorn.ModelError, match="the stack") as caught:
        run(m, ms(5))
    assert isinstance(caught.value.__cause__, LookupError)
    run(m, ms(5))


def test_a_handler_takes_over_task_failures_callbacks_and_hooks_but_not_the_bound(
    u575: popcorn.PartDescription,
) -> None:
    heard: list[str] = []

    class Raising(Stack):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            raise KeyError("hook says no")

    device = Raising()
    m = asleep(u575, {"USART1": [device]})

    def handler(loop: asyncio.AbstractEventLoop, context: dict[str, Any]) -> None:
        heard.append(str(context["exception"]))

    m.loop.set_exception_handler(handler)

    async def awaits_later(task: asyncio.Task[None]) -> None:
        await asyncio.sleep(0.002)
        with pytest.raises(LookupError):
            await task
        heard.append("awaited later")

    def raises() -> None:
        raise ValueError("callback says no")

    late = m.loop.create_task(fails_soon())
    m.loop.create_task(awaits_later(late))
    m.loop.call_soon(raises)
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(5))
    assert len(heard) == 4
    assert heard[0] == "callback says no"
    assert "hook says no" in heard[1]
    assert heard[2:] == ["task says no", "awaited later"]
    assert m.loop.get_exception_handler() is not None

    def chain() -> None:
        m.loop.call_soon(chain)

    m.loop.call_soon(chain)
    with pytest.raises(popcorn.ModelError, match="65536"):
        run(m, ms(1))


def test_a_task_awaited_at_a_later_step_raises_unless_a_handler_is_set(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []

    async def awaits_later(task: asyncio.Task[None]) -> None:
        await asyncio.sleep(0.002)
        try:
            await task
        except LookupError:
            out.append("awaited")

    m = asleep(u575)
    m.loop.create_task(awaits_later(m.loop.create_task(fails_soon())))
    with pytest.raises(popcorn.ModelError, match="nothing retrieved"):
        run(m, ms(5))
    run(m, ms(5))
    assert out == ["awaited"]


def test_a_raising_handler_stops_the_run(u575: popcorn.PartDescription) -> None:
    def handler(loop: asyncio.AbstractEventLoop, context: dict[str, Any]) -> None:
        raise OSError("handler says no")

    def raises() -> None:
        raise ValueError("callback")

    m = asleep(u575)
    m.loop.set_exception_handler(handler)
    m.loop.call_soon(raises)
    with pytest.raises(popcorn.ModelError, match="handler says no"):
        run(m, ms(1))


def test_every_loop_call_with_no_virtual_form_is_refused_by_name(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575)
    loop: Any = m.loop
    implemented = {
        "call_at",
        "call_exception_handler",
        "call_later",
        "call_soon",
        "close",
        "create_future",
        "create_task",
        "default_exception_handler",
        "get_debug",
        "get_exception_handler",
        "get_task_factory",
        "is_closed",
        "is_running",
        "set_debug",
        "set_exception_handler",
        "set_task_factory",
        "shutdown_asyncgens",
        "time",
    }
    names = [
        name
        for name, _ in inspect.getmembers(asyncio.AbstractEventLoop, inspect.isroutine)
        if not name.startswith("_") and name not in implemented
    ]
    assert len(names) >= 34
    no_argument = {"run_forever", "stop", "shutdown_default_executor"}
    for name in names:
        method = getattr(loop, name)
        with pytest.raises(VirtualLoopUnsupported, match=f"VirtualLoop.{name}"):
            result = method() if name in no_argument else method(None)
            if inspect.iscoroutine(result):
                result.send(None)
    assert issubclass(VirtualLoopUnsupported, NotImplementedError)
    for name in implemented:
        assert getattr(VirtualLoop, name) is not getattr(asyncio.AbstractEventLoop, name)


def test_a_coroutine_given_to_a_refused_call_is_closed(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)

    async def body() -> None:
        return None

    coroutine = body()
    with pytest.raises(VirtualLoopUnsupported):
        m.loop.run_until_complete(coroutine)
    assert inspect.getcoroutinestate(coroutine) == inspect.CORO_CLOSED


def test_asyncio_run_and_to_thread_are_refused_inside_a_step(
    u575: popcorn.PartDescription,
) -> None:
    caught: list[type[BaseException]] = []

    def build(device: Stack) -> None:
        async def body() -> None:
            try:
                await asyncio.to_thread(print)
            except VirtualLoopUnsupported as error:
                caught.append(type(error))

        asyncio.get_running_loop().create_task(body())
        try:
            coroutine = asyncio.sleep(0)
            try:
                asyncio.run(coroutine)
            finally:
                coroutine.close()
        except RuntimeError as error:
            caught.append(type(error))

    scenario(build)(u575)
    assert caught == [RuntimeError, VirtualLoopUnsupported]


var: contextvars.ContextVar[str] = contextvars.ContextVar("var", default="unset")


def test_every_hook_shares_one_context_and_a_task_gets_a_copy(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[tuple[str, str]] = []

    async def body() -> None:
        seen.append(("task", var.get()))
        var.set("changed by the task")

    class Contextual(Stack):
        def on_attach(self) -> None:
            seen.append(("attach", var.get()))
            var.set("set in on_attach")

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            seen.append(("frame", var.get()))
            self.loop.create_task(body())

    device = Contextual()
    var.set("at the machine")
    m = asleep(u575, {"USART1": [device]})
    var.set("after the machine")
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    m.periph.USART1.TDR.value = 0x42
    run(m, ms(1))
    assert seen == [
        ("attach", "at the machine"),
        ("frame", "set in on_attach"),
        ("task", "set in on_attach"),
        ("frame", "set in on_attach"),
        ("task", "set in on_attach"),
    ]
    assert var.get() == "after the machine"


def test_a_timer_armed_from_the_test_sees_the_context_of_the_test(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[str] = []
    m = asleep(u575)
    var.set("the test's")
    m.loop.call_later(0.001, lambda: seen.append(var.get()))
    var.set("later")
    run(m, ms(2))
    assert seen == ["the test's"]


def test_an_eager_task_factory_runs_a_task_with_nothing_to_await_twice(
    u575: popcorn.PartDescription,
) -> None:
    out: list[int] = []

    async def body(n: int) -> int:
        out.append(n)
        return n

    def build(device: Stack) -> None:
        loop = asyncio.get_running_loop()
        loop.set_task_factory(asyncio.eager_task_factory)
        assert loop.get_task_factory() is asyncio.eager_task_factory
        first = loop.create_task(body(1))
        assert first.done()
        second = loop.create_task(body(2))
        assert second.done()

    device = scenario(build)(u575)
    assert out == [1, 2]
    tracked = device.loop._tasks  # pyright: ignore[reportPrivateUsage]
    assert not tracked


def machine_with_pending_work(
    u575: popcorn.PartDescription, out: list[str]
) -> tuple[weakref.ref[popcorn.Machine], weakref.ref[object], weakref.ref[VirtualLoop]]:
    class Held:
        pass

    held = Held()

    async def generator() -> AsyncIterator[int]:
        try:
            yield 1
            yield 2
        finally:
            out.append("generator closed")

    async def pending(thing: object) -> None:
        iterator = generator()
        assert await anext(iterator) == 1
        try:
            await asyncio.sleep(3600)
        finally:
            out.append("task finally")
            del thing

    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    loop = m.loop
    loop.create_task(pending(held))
    loop.call_later(7200, print)
    run(m, ms(1))
    return weakref.ref(m), weakref.ref(held), weakref.ref(loop)


def test_a_dropped_machine_frees_all_of_it_and_runs_a_plain_finally_once(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    out: list[str] = []
    with caplog.at_level(logging.DEBUG, logger="asyncio"):
        machine, held, loop = machine_with_pending_work(u575, out)
        assert collected(machine)
        assert collected(held)
        assert collected(loop)
    assert out.count("task finally") == 1
    assert out.count("generator closed") == 1
    assert "destroyed" not in caplog.text
    assert "never retrieved" not in caplog.text


def test_the_finalizer_closes_a_loop_the_test_holds_past_its_dropped_machine(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            await asyncio.sleep(0)
            out.append("task finally")

    m = asleep(u575)
    loop = m.loop
    loop.create_task(pending())
    run(m, ms(1))
    machine = weakref.ref(m)
    del m
    assert collected(machine)
    assert loop.is_closed()
    assert out == ["task finally"]


class ServingStack(UartDevice):
    """The normal shape: a device's task with nothing of the machine in its frame."""

    def __init__(self, out: list[str]) -> None:
        super().__init__()
        self.out = out

    def on_attach(self) -> None:
        self.loop.create_task(self.serve())

    async def serve(self) -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            self.out.append("entered")
            await asyncio.sleep(0)
            self.out.append("done")


@pytest.mark.parametrize("repeat", range(8))
def test_close_before_dropping_runs_every_finally_to_its_end(
    u575: popcorn.PartDescription, repeat: int
) -> None:
    del repeat
    out: list[str] = []
    m = asleep(u575, {"USART1": [ServingStack(out)]})
    run(m, ms(1))
    m.loop.close()
    assert out == ["entered", "done"]
    machine = weakref.ref(m)
    del m
    assert collected(machine)
    assert out == ["entered", "done"]


def test_a_reset_from_a_finally_at_close_runs_its_hook_in_that_step(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[object] = []

    class Resetting(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            seen.append((kind, asyncio.get_running_loop() is self.loop, self.now.ps))

    async def resets_at_close() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            m.reset(popcorn.ResetKind.SOFTWARE)
            seen.append("after")

    device = Resetting()
    m = asleep(u575, {"USART1": [device]})
    m.loop.create_task(resets_at_close())
    m.loop.call_later(0.0005, lambda: None)
    run(m, ms(1))
    seen.clear()
    m.loop.close()
    assert seen == [(popcorn.ResetKind.SOFTWARE, True, 0), "after"]


def test_a_system_exit_from_a_callback_leaves_the_later_timers_armed(
    u575: popcorn.PartDescription,
) -> None:
    fired: list[int] = []

    def exits() -> None:
        raise SystemExit(3)

    m = asleep(u575)
    m.loop.call_later(0.001, exits)
    m.loop.call_later(0.002, lambda: fired.append(stamp(m.loop)))
    with pytest.raises(SystemExit):
        run(m, ms(5))
    run(m, ms(5))
    assert fired == [ms(2).ps]


def test_close_cancels_the_tasks_and_later_calls_raise(u575: popcorn.PartDescription) -> None:
    out: list[str] = []

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            out.append("finally")

    async def clings() -> None:
        try:
            await asyncio.sleep(3600)
        except asyncio.CancelledError:
            await asyncio.sleep(3600)

    m = asleep(u575)
    m.loop.create_task(pending())
    m.loop.create_task(clings())
    run(m, ms(1))
    m.loop.close()
    m.loop.close()
    assert m.loop.is_closed()
    assert out == ["finally"]
    with pytest.raises(RuntimeError, match="closed"):
        m.loop.call_soon(print)
    with pytest.raises(RuntimeError, match="closed"):
        m.loop.call_later(1, print)
    asyncio.run(m.run())
    assert m.last_stop is not None and m.last_stop.reason is popcorn.StopReason.IDLE
    gc.collect()


def test_a_task_raising_at_close_goes_to_the_log(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    async def raises_at_close() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            raise LookupError("at close")

    m = asleep(u575)
    m.loop.create_task(raises_at_close())
    run(m, ms(1))
    with caplog.at_level(logging.ERROR, logger="asyncio"):
        m.loop.close()
    assert "at close" in caplog.text


def test_a_finished_async_generator_is_dropped_and_their_number_is_bounded(
    u575: popcorn.PartDescription,
) -> None:
    async def generator() -> AsyncIterator[int]:
        yield 1

    async def body() -> None:
        for _ in range(1000):
            async for _ in generator():
                pass

    m = asleep(u575)
    m.loop.create_task(body())
    run(m, ms(1))
    held = m.loop._asyncgens  # pyright: ignore[reportPrivateUsage]
    assert len(held) <= 257


def test_cancelled_timers_do_not_pile_up(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)
    m.loop.call_later(3000, print)

    async def body() -> None:
        for _ in range(1000):
            await asyncio.wait_for(asyncio.sleep(0), 3600)
            await asyncio.sleep(1e-6)

    task = m.loop.create_task(body())
    run(m, ms(5))
    assert task.done()
    timers = m.loop._timers  # pyright: ignore[reportPrivateUsage]
    assert len(timers) <= 202


def test_a_snapshot_leaves_the_armed_event_out_and_the_timer_goes_on(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    fired: list[int] = []
    m = asleep(u575)
    m.loop.call_later(0.001, lambda: fired.append(stamp(m.loop)))
    armed = m.snapshot()
    path = tmp_path / "armed.snapshot"
    armed.save(path)
    run(m, us(10))
    m.restore(armed)
    assert stamp(m.loop) == m.time.ps
    run(m, ms(2))
    assert fired == [ms(1).ps]
    other = popcorn.Machine(u575)
    other.restore(other.load_snapshot(path))
    run(other, ms(2))
    assert len(fired) == 1


def test_a_stamp_after_a_restore_counts_from_the_restored_clock(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    saved = m.snapshot()
    device.loop.call_later(0.0005, device.mark, "later than the frame")
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    frame = device.log[0]
    assert [name for name, _, _ in device.log] == ["frame 0x41", "later than the frame"]
    device.log.clear()
    m.restore(saved)
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert device.log == [frame]


def test_a_hook_stamp_behind_the_last_one_keeps_time_monotonic(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    loop = device.loop
    run_hook = loop._run_hook  # pyright: ignore[reportPrivateUsage]
    run(m, us(100))
    later = m.time.ps
    run_hook(later, device.mark, "first")
    run_hook(later - 1, device.mark, "behind")
    assert device.log == [("first", later, later), ("behind", later, later)]


def abort_and_timer(u575: popcorn.PartDescription, nops: int) -> list[tuple[str, int, int]]:
    device = Stack()
    m = asleep(u575, {"USART1": [device]}, BRANCH_TO_SELF)
    uart = m.periph.USART1
    base = next(p.base for p in u575.peripherals if p.name == "USART1")
    offset = uart.CR1_disabled.offset
    code = NOP * nops + UE_CLEAR_AND_SET + (base + offset).to_bytes(4, "little") + UE_TE_RE
    m.mem.write(RAM + 0x100, code)
    uart.TDR.value = 0x41
    start = m.time.ps
    timer_due = start + us(5).ps
    m.loop.call_at(timer_due / PS_PER_SECOND, lambda: device.mark("timer"))
    m.reg[Reg.PC] = RAM + 0x100
    run(m, us(200))
    return device.log


def test_a_firmware_abort_runs_after_a_timer_due_inside_its_block(
    u575: popcorn.PartDescription,
) -> None:
    short = abort_and_timer(u575, 2)
    long = abort_and_timer(u575, 200)
    for log in (short, long):
        stamps = [at for _, at, _ in log]
        assert stamps == sorted(stamps)
        assert all(at == now for _, at, now in log)
        names = [name for name, _, _ in log]
        assert sorted(names) == ["ABORTED", "timer"]
    assert [entry for entry in short if entry[0] == "timer"] == [
        entry for entry in long if entry[0] == "timer"
    ]
    assert [name for name, _, _ in short] == ["ABORTED", "timer"]
    assert [name for name, _, _ in long] == ["timer", "ABORTED"]


def test_a_parked_abort_runs_on_the_machine_thread_at_its_stamp(
    u575: popcorn.PartDescription,
) -> None:
    threads: list[bool] = []

    class Noting(Stack):
        def on_error(self, kind: UartError, at: VirtualTime) -> None:
            super().on_error(kind, at)
            threads.append(_core.on_host_thread())

    device = Noting()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, us(5))
    written = m.time.ps
    m.periph.USART1.CR1_enabled.UE = 0
    assert device.log == []
    run(m, us(100))
    assert device.log == [("ABORTED", written, written)]
    assert threads == [True]


def wake_on_abort(u575: popcorn.PartDescription, *, firmware: bool) -> tuple[bool, bool, bool]:
    """A task waits on a future `on_error` sets, then sends 100 us after the abort. Whether it ran
    on the machine's thread, and whether the reply is in the receiver 2 instructions before and
    after its end."""
    at_ps: list[int] = []
    threads: list[bool] = []

    class Waking(Stack):
        def __init__(self) -> None:
            super().__init__()
            self.waiter: asyncio.Future[int] | None = None

        def on_attach(self) -> None:
            self.waiter = self.loop.create_future()
            self.loop.create_task(self.waits())

        async def waits(self) -> None:
            assert self.waiter is not None
            at = await self.waiter
            threads.append(_core.on_host_thread())
            at_ps.append(at)
            self.send(b"\x55", at=VirtualTime(at) + us(100))

        def on_error(self, kind: UartError, at: VirtualTime) -> None:
            assert self.waiter is not None
            self.waiter.set_result(at.ps)

    device = Waking()
    m = asleep(u575, {"USART1": [device]}, BRANCH_TO_SELF)
    uart = m.periph.USART1
    run(m, us(1))
    uart.TDR.value = 0x41
    run(m, us(5))
    if firmware:
        base = next(p.base for p in u575.peripherals if p.name == "USART1")
        offset = uart.CR1_disabled.offset
        m.mem.write(RAM, UE_CLEAR_AND_SET + (base + offset).to_bytes(4, "little") + UE_TE_RE)
        m.reg[Reg.PC] = RAM
    else:
        uart.CR1_enabled.UE = 0
        uart.CR1_disabled.poke(TE=1, RE=1)
        uart.CR1_disabled.UE = 1
    run(m, us(5))
    due = at_ps[0] + us(100).ps + FRAME_PS
    run(m, ps(due - 2 * STEP_PS - m.time.ps))
    soon = uart.ISR_enabled.RXFNE != 0
    run(m, ps(4 * STEP_PS))
    return all(threads) and len(threads) == 1, soon, uart.ISR_enabled.RXFNE != 0


def test_a_task_woken_by_a_parked_and_a_firmware_abort_sends_the_same_way(
    u575: popcorn.PartDescription,
) -> None:
    assert wake_on_abort(u575, firmware=False) == (True, False, True)
    assert wake_on_abort(u575, firmware=True) == (True, False, True)


def test_a_timer_does_not_move_a_frame_of_the_firmware(u575: popcorn.PartDescription) -> None:
    def frames(tick_ps: int) -> list[tuple[str, int, int]]:
        device = Stack()
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
        m.reset()
        if tick_ps:

            def tick() -> None:
                m.loop.call_later(tick_ps / PS_PER_SECOND, tick)

            m.loop.call_soon(tick)
        run(m, ms(20))
        return device.log

    plain = frames(0)
    assert plain == frames(1_000_000)
    assert plain == frames(13_000_000)


def mixed_run(
    u575: popcorn.PartDescription, code: bytes
) -> tuple[list[tuple[object, ...]], list[int]]:
    """Three frames queued at time 0 towards a peer of timers, timeouts and replies."""
    log: list[tuple[object, ...]] = []
    sent: list[int] = []

    class Peer(UartDevice):
        def on_attach(self) -> None:
            self.queue: asyncio.Queue[int] = asyncio.Queue()
            asyncio.get_running_loop().create_task(self.serve())
            asyncio.get_running_loop().create_task(self.beacon())

        async def serve(self) -> None:
            while True:
                try:
                    byte = await asyncio.wait_for(self.queue.get(), 0.0003)
                except TimeoutError:
                    log.append(("timeout", self.now.ps, stamp(self.loop)))
                    continue
                await asyncio.sleep(0.0001)
                log.append(("reply", byte, self.now.ps))
                self.send(bytes([byte + 1]), at=self.now)

        async def beacon(self) -> None:
            while True:
                await asyncio.sleep(0.00025)
                log.append(("beacon", self.now.ps))

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            log.append(("frame", byte, at.ps, self.now.ps))
            sent.append(byte)
            self.queue.put_nowait(byte)

    m = asleep(u575, {"USART1": [Peer()]}, code)
    uart = m.periph.USART1
    uart.CR1_enabled.poke(FIFOEN=1)
    for byte in (0x41, 0x42, 0x43):
        uart.TDR.value = byte
    run(m, ms(3))
    return log, sent


def test_two_runs_at_any_block_shape_give_the_same_bytes_and_stamps(
    u575: popcorn.PartDescription,
) -> None:
    first = mixed_run(u575, nop_loop(2))
    assert first[1] == [0x41, 0x42, 0x43]
    assert {entry[0] for entry in first[0]} == {"frame", "reply", "timeout", "beacon"}
    assert first == mixed_run(u575, nop_loop(2))
    assert first == mixed_run(u575, nop_loop(40))
    assert first == mixed_run(u575, BRANCH_TO_SELF)
    assert first == mixed_run(u575, WFI_LOOP)


def test_machine_keeps_no_loop_until_first_use(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)
    assert m._loop is None  # pyright: ignore[reportPrivateUsage]
    loop = m.loop
    assert m.loop is loop
    assert loop.time() == m.time.ps / PS_PER_SECOND
    assert repr(loop) == "<VirtualLoop of STM32U575QGIx>"


def test_a_device_with_no_live_machine_has_no_loop(u575: popcorn.PartDescription) -> None:
    device = Stack()
    with pytest.raises(popcorn.ConfigError):
        _ = device.loop
    m = asleep(u575, {"USART1": [device]})
    assert device.loop is m.loop
    watch = weakref.ref(m)
    del m
    assert collected(watch)
    with pytest.raises(popcorn.ConfigError):
        _ = device.loop


def test_on_attach_after_a_restore_is_stamped_at_the_restored_time(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    def build(device: Stack) -> None:
        device.mark("attach")

    first = Stack(build)
    m = asleep(u575, {"USART1": [first]}, nop_loop(40))
    run(m, us(100))
    saved = m.time.ps
    path = tmp_path / "attached.snapshot"
    m.snapshot().save(path)
    second = Stack(build)
    other = popcorn.Machine(u575, devices={"USART1": [second]})
    other.restore(other.load_snapshot(path))
    run(other, us(100))
    assert second.log == [("attach", saved, saved)]


def test_a_timer_due_at_a_frame_end_runs_after_the_frame_and_time_does_not_go_back(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    start = m.time.ps
    end = start + FRAME_PS
    device.loop.call_at((end - 1) / PS_PER_SECOND, device.mark, "just before")
    device.loop.call_at(end / PS_PER_SECOND, device.mark, "at")
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert device.log == [
        ("frame 0x41", end, end),
        ("just before", end, end),
        ("at", end, end),
    ]


def test_a_parked_call_at_in_the_past_keeps_the_clock_of_the_call(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]}, nop_loop(40))
    run(m, us(100))
    parked = m.time.ps
    device.loop.call_at(0.0, device.mark, "past")
    run(m, us(100))
    assert device.log == [("past", parked, parked)]


def test_a_future_never_retrieved_is_logged_and_stops_no_run(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    m = asleep(u575)
    future = m.loop.create_future()
    future.set_exception(LookupError("nobody asks"))
    del future
    with caplog.at_level(logging.ERROR, logger="asyncio"):
        gc.collect()
    assert "never retrieved" in caplog.text
    run(m, ms(1))


def test_close_closes_an_async_generator_left_unfinished(u575: popcorn.PartDescription) -> None:
    out: list[str] = []
    held: list[object] = []

    async def generator() -> AsyncIterator[int]:
        try:
            yield 1
            yield 2
        finally:
            await asyncio.sleep(0)
            out.append("closed")

    async def body() -> None:
        iterator = generator()
        held.append(iterator)
        assert await anext(iterator) == 1

    m = asleep(u575)
    m.loop.create_task(body())
    run(m, ms(1))
    assert out == []
    m.loop.close()
    assert out == ["closed"]


def test_shutdown_asyncgens_closes_them_from_a_task(u575: popcorn.PartDescription) -> None:
    out: list[str] = []

    async def generator() -> AsyncIterator[int]:
        try:
            yield 1
            yield 2
        finally:
            out.append("closed")

    async def body() -> None:
        iterator = generator()
        assert await anext(iterator) == 1
        await m.loop.shutdown_asyncgens()
        out.append(str(len(m.loop._asyncgens)))  # pyright: ignore[reportPrivateUsage]
        del iterator

    m = asleep(u575)
    m.loop.create_task(body())
    run(m, ms(1))
    assert out == ["closed", "0"]


def rts_and_frame_at_one_count(
    u575: popcorn.PartDescription, gap_ps: int
) -> tuple[list[tuple[str, int, int]], list[bytes]]:
    """A device frame starts `gap_ps` after 1 us, the firmware's at 2 us, both 87.5 us long. The
    receiver's RTS falls at the first's end and replies at its `at`."""
    log: list[tuple[str, int, int]] = []
    replied: list[bytes] = []

    class Duplex(UartDevice):
        def on_attach(self) -> None:
            self.send(b"x", at=self.now + ps(us(1).ps + gap_ps))

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            log.append(("frame", at.ps, self.now.ps))

        def on_rts(self, ready: bool, at: VirtualTime) -> None:
            log.append(("rts", at.ps, self.now.ps))
            if not ready:
                self.send(b"y", at=at, obey_rts=False)
                replied.append(b"y")

    m = asleep(u575, {"USART1": [Duplex()]}, rtse=True)
    run(m, us(2))
    assert m.time.ps == us(2).ps
    m.periph.USART1.TDR.value = 0x41
    run(m, us(100))
    return log, replied


def test_a_hook_held_at_the_last_stamp_of_its_instruction_count_can_reply_at_its_at(
    u575: popcorn.PartDescription,
) -> None:
    log, replied = rts_and_frame_at_one_count(u575, 900_000)
    assert log[:2] == [("frame", 89_500_000, 89_500_000), ("rts", 89_500_000, 89_500_000)]
    assert replied == [b"y"]
    log, replied = rts_and_frame_at_one_count(u575, 1_100_000)
    assert log[:2] == [("frame", 89_500_000, 89_500_000), ("rts", 89_600_000, 89_600_000)]
    assert replied == [b"y"]


def test_a_parked_abort_then_a_reset_drops_on_error_and_reset_tells_the_device(
    u575: popcorn.PartDescription,
) -> None:
    resets: list[popcorn.ResetKind] = []

    class Noting(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            resets.append(kind)

    device = Noting()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, us(5))
    resets.clear()
    m.periph.USART1.CR1_enabled.UE = 0
    m.reset(popcorn.ResetKind.SOFTWARE)
    parked_on(m)
    run(m, us(50))
    assert device.log == []
    assert resets == [popcorn.ResetKind.SOFTWARE]


def firmware_of_two_frames(u575: popcorn.PartDescription, m: popcorn.Machine) -> bytes:
    """A frame, a wait past its end, a second frame, then a UE clear that cuts it."""
    uart = m.periph.USART1
    base = next(p.base for p in u575.peripherals if p.name == "USART1")
    tdr = (base + uart.TDR.offset).to_bytes(4, "little")
    first = bytes.fromhex("014a" "4123" "1360" "01e0") + tdr
    wait_then_second = bytes.fromhex("c824" "013c" "fdd1" "1360")
    cut = UE_CLEAR_AND_SET + (base + uart.CR1_disabled.offset).to_bytes(4, "little") + UE_TE_RE
    return NOP * 20 + first + wait_then_second + NOP * 2 + cut


def sliced(
    u575: popcorn.PartDescription, goals: list[int], *, timers: bool
) -> list[tuple[str, int, int]]:
    device = Stack()
    m = asleep(u575, {"USART1": [device]}, BRANCH_TO_SELF)
    m.mem.write(RAM, firmware_of_two_frames(u575, m))
    m.reg[Reg.PC] = RAM
    if timers:
        for n in range(1, 5):
            device.loop.call_later(n * 3.1e-5, device.mark, f"timer {n}")
    for goal in goals:
        run(m, ps(goal))
    return device.log


def test_one_firmware_gives_the_same_stamps_at_any_slicing(u575: popcorn.PartDescription) -> None:
    one = [us(200).ps]
    small = [us(1).ps] * 200
    uneven = [6_900_000] * 29
    for timers in (False, True):
        log = sliced(u575, one, timers=timers)
        assert log == sliced(u575, small, timers=timers)
        assert log == sliced(u575, uneven, timers=timers)
        names = [name for name, _, _ in log]
        assert "frame 0x41" in names and "ABORTED" in names
    plain = sliced(u575, one, timers=False)
    timed = sliced(u575, one, timers=True)
    assert plain == [entry for entry in timed if not entry[0].startswith("timer")]


def test_two_devices_share_the_loop_and_one_log(u575: popcorn.PartDescription) -> None:
    log: list[tuple[str, int, int]] = []

    class Named(UartDevice):
        def __init__(self, name: str) -> None:
            super().__init__()
            self.name = name

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            log.append((f"{self.name} {byte:#x}", at.ps, self.now.ps))
            self.loop.call_later(
                1e-6, lambda: log.append((f"{self.name} timer", stamp(self.loop), 0))
            )

    first = Named("one")
    second = Named("two")
    m = asleep(u575, {"USART1": [first], "USART2": [second]})
    assert first.loop is second.loop is m.loop
    m.periph.RCC.RCC_APB1ENR1.USART2EN = 1
    m.periph.USART2.BRR.value = BRR * 2
    m.periph.USART2.CR1_disabled.poke(UE=1, TE=1, RE=1)
    m.periph.USART1.TDR.value = 0x41
    m.periph.USART2.TDR.value = 0x42
    run(m, ms(1))
    assert [name for name, _, _ in log] == ["one 0x41", "one timer", "two 0x42", "two timer"]
    assert [at for _, at, _ in log] == [
        FRAME_PS,
        FRAME_PS + us(1).ps,
        2 * FRAME_PS,
        2 * FRAME_PS + us(1).ps,
    ]
    assert all(now == at for name, at, now in log if "timer" not in name)


def two_uarts(
    u575: popcorn.PartDescription, first: UartDevice, second: UartDevice
) -> popcorn.Machine:
    m = asleep(u575, {"USART1": [first], "USART2": [second]})
    m.periph.RCC.RCC_APB1ENR1.USART2EN = 1
    m.periph.USART2.BRR.value = BRR
    m.periph.USART2.CR1_disabled.poke(UE=1, TE=1, RE=1)
    return m


async def parser_fails() -> None:
    raise LookupError("parser says no")


def test_a_retrieval_only_by_another_step_at_the_same_picosecond_is_too_late(
    u575: popcorn.PartDescription,
) -> None:
    parsers: list[asyncio.Task[None]] = []
    heard: list[tuple[str, int]] = []

    class Starting(UartDevice):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            heard.append(("starts", at.ps))
            parsers.append(self.loop.create_task(parser_fails(), name="the parser"))

    class Awaiting(UartDevice):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            heard.append(("awaits", at.ps))
            self.loop.create_task(self.retrieves())

        async def retrieves(self) -> None:
            with pytest.raises(LookupError):
                await parsers[0]
            heard.append(("retrieved", self.now.ps))

    m = two_uarts(u575, Starting(), Awaiting())
    m.periph.USART1.TDR.value = 0x41
    m.periph.USART2.TDR.value = 0x42
    with pytest.raises(popcorn.ModelError, match="the parser"):
        run(m, ms(1))
    assert heard == [("starts", FRAME_PS)]
    run(m, ms(1))
    assert heard == [("starts", FRAME_PS), ("awaits", FRAME_PS), ("retrieved", FRAME_PS)]


async def parser_fails_after_a_step() -> None:
    await asyncio.sleep(0)
    raise LookupError("parser says no")


def test_an_awaiter_already_linked_wakes_inside_the_step_and_retrieves_in_time(
    u575: popcorn.PartDescription,
) -> None:
    heard: list[tuple[str, int]] = []
    parsed: list[asyncio.Future[asyncio.Task[None]]] = []

    class Starting(UartDevice):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            parsed[0].set_result(self.loop.create_task(parser_fails_after_a_step()))

    class Linked(UartDevice):
        def on_attach(self) -> None:
            parsed.append(self.loop.create_future())
            self.loop.create_task(self.retrieves())

        async def retrieves(self) -> None:
            task = await parsed[0]
            with pytest.raises(LookupError):
                await task
            heard.append(("retrieved", self.now.ps))

    m = two_uarts(u575, Starting(), Linked())
    run(m, us(1))
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert len(heard) == 1
    assert heard[0][0] == "retrieved"


def test_a_collection_inside_the_first_use_runs_a_finalizer_that_makes_a_first_use(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    out: list[str] = []
    fresh = asleep(u575)
    other = asleep(u575)

    class Collecting(VirtualLoop):
        def __init__(self, name: str) -> None:
            gc.collect()
            super().__init__(name)

    async def reaches_the_machine() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            out.append(repr(fresh.loop))

    def dropped_with_its_task() -> None:
        m = asleep(u575)
        m.loop.create_task(reaches_the_machine())
        run(m, ms(1))

    made: list[VirtualLoop] = []
    gc.disable()
    try:
        dropped_with_its_task()
        monkeypatch.setattr(popcorn._machine, "VirtualLoop", Collecting)  # type: ignore[attr-defined]
        held = popcorn._machine._FIRST_LOOP  # type: ignore[attr-defined]
        monkeypatch.setattr(popcorn._machine, "_FIRST_LOOP", type(held)())  # type: ignore[attr-defined]
        first_use = threading.Thread(target=lambda: made.append(other.loop), daemon=True)
        first_use.start()
        first_use.join(10)
    finally:
        gc.enable()
    assert not first_use.is_alive()
    assert made == [other.loop]
    assert out == ["<VirtualLoop of STM32U575QGIx>"]


def test_two_threads_at_the_first_use_get_one_loop(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    both = threading.Barrier(2)

    class Racing(VirtualLoop):
        def __init__(self, name: str) -> None:
            with contextlib.suppress(threading.BrokenBarrierError):
                both.wait(0.2)
            super().__init__(name)

    monkeypatch.setattr(popcorn._machine, "VirtualLoop", Racing)  # type: ignore[attr-defined]
    m = asleep(u575)
    made: list[VirtualLoop] = []
    threads = [threading.Thread(target=lambda: made.append(m.loop), daemon=True) for _ in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(10)
    assert len(made) == 2
    assert made[0] is made[1]
