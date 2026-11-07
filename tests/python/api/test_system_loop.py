"""The virtual-time loop of a System member: its own loop, the same bytes and stamps at every
quantum as the same firmware alone."""

from __future__ import annotations

import asyncio
import os
import threading
import weakref
from collections.abc import Callable, Mapping, Sequence

import popcorn
import pytest
from popcorn import System, UartLink
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
LINK_LATENCY = us(20)
BAUD = 114_285
QUANTA = (None, 5, 1)

type Log = list[tuple[str, int, int]]


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


def uart_on(m: popcorn.Machine, name: str = "USART1", *, rtse: bool = False) -> None:
    uart = m.periph[name]
    if name == "USART1":
        m.periph.RCC.APB2ENR.USART1EN = 1
    else:
        setattr(m.periph.RCC.RCC_APB1ENR1, f"{name}EN", 1)
    uart.BRR.value = BRR
    if rtse:
        uart.CR3.RTSE = 1
    uart.CR1_disabled.poke(UE=1, TE=1, RE=1)


def member(
    u575: popcorn.PartDescription,
    name: str,
    devices: Mapping[str, Sequence[UartDevice]] | None = None,
    code: bytes = WFI_LOOP,
    *,
    rtse: bool = False,
) -> popcorn.Machine:
    m = popcorn.Machine(u575, devices=devices, name=name)
    m.reset()
    parked_on(m, code)
    uart_on(m, rtse=rtse)
    uart_on(m, "USART2")
    uart_on(m, "USART3")
    return m


def links(third: int | None) -> list[UartLink]:
    made = [UartLink("a.USART2", "b.USART2", latency=LINK_LATENCY, baud=BAUD)]
    if third is not None:
        made.append(UartLink("a.USART3", "b.USART3", latency=us(third), baud=BAUD))
    return made


class Stack(UartDevice):
    """Logs what it hears with `now`, and runs `build` inside `on_attach`."""

    def __init__(self, build: Callable[[Stack], object] | None = None) -> None:
        super().__init__()
        self.build = build
        self.log: Log = []

    def on_attach(self) -> None:
        if self.build is not None:
            self.build(self)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.log.append((f"frame {byte:#x}", at.ps, self.now.ps))

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.log.append((kind.name, at.ps, self.now.ps))

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        self.log.append((f"rts {ready}", at.ps, self.now.ps))

    def mark(self, what: str) -> None:
        self.log.append((what, stamp(self.loop), self.now.ps))


def firmware(u575: popcorn.PartDescription, m: popcorn.Machine) -> bytes:
    """Sends a frame, waits past its end and past the device's, reads the receiver so RTS rises,
    echoes what it read, waits, sends a last frame, then a UE clear cuts it."""
    uart = m.periph.USART1
    base = next(p.base for p in u575.peripherals if p.name == "USART1")
    tdr = (base + uart.TDR.offset).to_bytes(4, "little")
    rdr = (base + uart.RDR.offset).to_bytes(4, "little")
    first = bytes.fromhex("014a" "4123" "1360" "01e0") + tdr
    wait = bytes.fromhex("c824" "013c" "fdd1" "00bf")
    read = bytes.fromhex("014d" "2b68" "02e0" "00bf") + rdr
    echo = bytes.fromhex("1360" "00bf")
    second = bytes.fromhex("4123" "1360")
    cut = UE_CLEAR_AND_SET + (base + uart.CR1_disabled.offset).to_bytes(4, "little") + UE_TE_RE
    return NOP * 20 + first + wait + read + echo + wait + second + cut


def sent_and_timed(device: Stack) -> None:
    device.send(b"\x55", at=device.now + us(1))
    for n in range(1, 13):
        device.loop.call_later(n * 3.1e-5, device.mark, f"timer {n}")


def scene_log(
    u575: popcorn.PartDescription, third: int | str | None, slice_instructions: int = 1_000_000
) -> Log:
    """The scene alone with `third` "alone", else on member a of a System of quantum `third`."""
    device = Stack(sent_and_timed)
    a = popcorn.Machine(
        u575, devices={"USART1": [device]}, name="a", slice_instructions=slice_instructions
    )
    a.reset()
    parked_on(a, BRANCH_TO_SELF)
    uart_on(a, rtse=True)
    uart_on(a, "USART2")
    uart_on(a, "USART3")
    a.mem.write(RAM, firmware(u575, a))
    if third == "alone":
        asyncio.run(a.run(for_=us(500)))
        return device.log
    b = member(u575, "b")
    assert not isinstance(third, str)
    with System({"a": a, "b": b}, links(third), slice_instructions=slice_instructions) as system:
        asyncio.run(system.run(for_=us(500)))
    return device.log


def test_stamps_at_each_quantum_equal_the_same_firmware_alone(
    u575: popcorn.PartDescription,
) -> None:
    alone = scene_log(u575, "alone")
    names = {name for name, _, _ in alone}
    expected = {
        "frame 0x41",
        "frame 0x55",
        "rts False",
        "rts True",
        "ABORTED",
        "timer 1",
        "timer 4",
    }
    assert expected <= names
    assert all(at == now for _, at, now in alone)
    for third in QUANTA:
        assert scene_log(u575, third) == alone
    assert scene_log(u575, 5, 7_777) == alone
    assert scene_log(u575, "alone", 7_777) == alone


def test_the_quanta_of_the_comparison_differ(u575: popcorn.PartDescription) -> None:
    seen: list[int] = []
    for third in QUANTA:
        with System({"a": member(u575, "a"), "b": member(u575, "b")}, links(third)) as system:
            seen.append(system.quantum.ps)
    assert seen == [us(20).ps, us(5).ps, us(1).ps]


def test_each_member_has_its_own_loop_and_its_own_thread(u575: popcorn.PartDescription) -> None:
    threads: dict[str, set[int]] = {"a": set(), "b": set()}
    first = Stack()
    second = Stack()
    a = member(u575, "a", {"USART1": [first]})
    b = member(u575, "b", {"USART1": [second]})
    assert a.loop is first.loop
    assert b.loop is second.loop
    assert a.loop is not b.loop

    def tick(name: str, loop: asyncio.AbstractEventLoop) -> None:
        assert asyncio.get_running_loop() is loop
        threads[name].add(threading.get_ident())
        loop.call_later(3.7e-5, tick, name, loop)

    a.loop.call_later(1e-5, tick, "a", a.loop)
    b.loop.call_later(1e-5, tick, "b", b.loop)
    with System({"a": a, "b": b}, links(5)) as system:
        asyncio.run(system.run(for_=us(300)))
    assert len(threads["a"]) == len(threads["b"]) == 1
    assert threads["a"].isdisjoint(threads["b"])


def timer_log(u575: popcorn.PartDescription, third: int | None) -> dict[str, list[int]]:
    """A machine with armed timers joins a System, one chain of timers on each member."""
    out: dict[str, list[int]] = {"a": [], "b": []}
    a = member(u575, "a", code=nop_loop(40))
    b = member(u575, "b", code=nop_loop(40))

    def tick(name: str, loop: asyncio.AbstractEventLoop) -> None:
        out[name].append(stamp(loop))
        if len(out[name]) < 6:
            loop.call_later(3.7e-5, tick, name, loop)

    a.loop.call_later(1e-5, tick, "a", a.loop)
    b.loop.call_later(1.3e-5, tick, "b", b.loop)
    with System({"a": a, "b": b}, links(third)) as system:
        asyncio.run(system.run(for_=us(300)))
    return out


def test_a_machine_with_an_armed_loop_joins_a_system_at_any_quantum(
    u575: popcorn.PartDescription,
) -> None:
    log = timer_log(u575, None)
    assert log["a"] == [10_000_000 + n * 37_000_000 for n in range(6)]
    assert log["b"] == [13_000_000 + n * 37_000_000 for n in range(6)]
    assert timer_log(u575, 5) == log
    assert timer_log(u575, 1) == log


def parked_calls(u575: popcorn.PartDescription, third: int) -> tuple[list[int], int]:
    """Stops a System at 50 us with the core in a loop of blocks, takes a `call_soon` parked on
    member a, runs on."""
    fired: list[int] = []
    a = member(u575, "a", code=nop_loop(40))
    b = member(u575, "b", code=nop_loop(40))
    with System({"a": a, "b": b}, links(third)) as system:
        asyncio.run(system.run(for_=us(50)))
        parked = a.time.ps
        a.loop.call_soon(lambda: fired.append(stamp(a.loop)))
        asyncio.run(system.run(for_=us(50)))
    return fired, parked


def test_a_parked_call_soon_on_a_member_fires_in_the_next_run_at_its_clock(
    u575: popcorn.PartDescription,
) -> None:
    fired, parked = parked_calls(u575, 20)
    assert fired == [parked]
    assert parked > us(50).ps
    assert parked_calls(u575, 5) == (fired, parked)
    assert parked_calls(u575, 1) == (fired, parked)


def test_the_abort_stands_between_timers_at_each_quantum(u575: popcorn.PartDescription) -> None:
    for third in QUANTA:
        names = [name for name, _, _ in scene_log(u575, third)]
        abort = names.index("ABORTED")
        assert names.index("timer 6") < abort < names.index("timer 8")


def test_a_callback_touching_another_member_raises_machine_running(
    u575: popcorn.PartDescription,
) -> None:
    caught: list[type[BaseException]] = []
    a = member(u575, "a")
    b = member(u575, "b")

    def touches() -> None:
        for reach in (lambda: b.time, lambda: b.loop.call_later(1e-6, print), b.loop.time):
            try:
                reach()
            except popcorn.MachineRunning as error:
                caught.append(type(error))

    a.loop.call_later(5e-5, touches)
    with System({"a": a, "b": b}, links(5)) as system:
        asyncio.run(system.run(for_=us(200)))
    assert caught == [popcorn.MachineRunning] * 3


def test_a_raising_timer_on_a_member_names_it_and_the_system_closes_with_timers_pending(
    u575: popcorn.PartDescription,
) -> None:
    fired: list[int] = []
    a = member(u575, "a")
    b = member(u575, "b")

    def raises() -> None:
        raise LookupError("no such entry")

    a.loop.call_later(5e-5, raises)
    a.loop.call_later(1e-3, lambda: fired.append(stamp(a.loop)))
    b.loop.call_later(1e-3, lambda: fired.append(stamp(b.loop)))
    system = System({"a": a, "b": b}, links(5))
    with pytest.raises(popcorn.ModelError, match="no such entry") as caught:
        asyncio.run(system.run(for_=us(200)))
    assert any("member a" in note for note in caught.value.__notes__)
    system.close()
    asyncio.run(a.run(for_=ms(2)))
    assert fired == [ms(1).ps]


def member_of_a_dropped_system(
    u575: popcorn.PartDescription, out: list[str]
) -> tuple[weakref.ref[popcorn.Machine], asyncio.AbstractEventLoop]:
    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            await asyncio.sleep(0)
            out.append("task finally")

    a = member(u575, "a")
    b = member(u575, "b")
    a.loop.create_task(pending())
    with System({"a": a, "b": b}, links(5)) as system:
        asyncio.run(system.run(for_=us(50)))
    return weakref.ref(a), a.loop


def test_the_finalizer_closes_the_loop_of_a_member_dropped_after_its_system(
    u575: popcorn.PartDescription,
) -> None:
    out: list[str] = []
    machine, loop = member_of_a_dropped_system(u575, out)
    assert collected(machine)
    assert loop.is_closed()
    assert out == ["task finally"]


def held_at_beside_a_link_arrival(
    u575: popcorn.PartDescription, third: int
) -> tuple[Log, list[int]]:
    """A's frame from the device and its firmware's frame end one count apart at most, and a frame
    from member b lands on a's linked USART2 in that count."""
    log: Log = []

    class Duplex(UartDevice):
        def on_attach(self) -> None:
            self.send(b"x", at=self.now + ps(19_900_000))

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            log.append(("frame", at.ps, self.now.ps))

        def on_rts(self, ready: bool, at: VirtualTime) -> None:
            log.append(("rts", at.ps, self.now.ps))
            if not ready:
                self.send(b"y", at=at, obey_rts=False)

    device = Duplex()
    a = member(u575, "a", {"USART1": [device]}, rtse=True)
    b = member(u575, "b")
    a.loop.call_at(107.5e-6, lambda: log.append(("timer", stamp(a.loop), device.now.ps)))
    with System({"a": a, "b": b}, links(third)) as system:
        tap = system.tap("a.USART2")
        b.periph.USART2.TDR.value = 0x77
        asyncio.run(system.run(for_=us(20)))
        assert a.time.ps == us(20).ps
        a.periph.USART1.TDR.value = 0x41
        asyncio.run(system.run(for_=us(200)))
        return log, [frame.arrival.ps for frame in tap.frames()]


def test_a_held_at_beside_a_link_arrival_in_its_count_is_the_same_at_any_quantum(
    u575: popcorn.PartDescription,
) -> None:
    log, frames = held_at_beside_a_link_arrival(u575, 20)
    assert log[:3] == [
        ("frame", 107_500_000, 107_500_000),
        ("timer", 107_500_000, 107_500_000),
        ("rts", 107_500_000, 107_500_000),
    ]
    assert frames == [107_500_000]
    assert held_at_beside_a_link_arrival(u575, 5) == (log, frames)
    assert held_at_beside_a_link_arrival(u575, 1) == (log, frames)


def timer_at_195(u575: popcorn.PartDescription, third: int | None) -> list[tuple[str, int]]:
    """A timer due inside the last quantum of a TIME pause at 200 us, a parked `call_soon`, and a
    timer placed from it. On a lone machine with `third` None, else on member a of a System."""
    log: list[tuple[str, int]] = []
    a = member(u575, "a", code=nop_loop(40))

    def parked() -> None:
        log.append(("parked", stamp(a.loop)))
        a.loop.call_later(1e-5, lambda: log.append(("placed", stamp(a.loop))))

    a.loop.call_later(195e-6, lambda: log.append(("timer", stamp(a.loop))))
    if third is None:
        asyncio.run(a.run(for_=us(200)))
        assert a.time.ps == us(205).ps
        a.loop.call_soon(parked)
        asyncio.run(a.run(for_=us(100)))
        return log
    b = member(u575, "b", code=nop_loop(40))
    with System({"a": a, "b": b}, links(third)) as system:
        asyncio.run(system.run(for_=us(200)))
        assert a.time.ps == us(205).ps
        a.loop.call_soon(parked)
        asyncio.run(system.run(for_=us(100)))
    return log


def test_a_parked_call_soon_keeps_its_stamp_and_order_at_each_quantum(
    u575: popcorn.PartDescription,
) -> None:
    lone = timer_at_195(u575, None)
    assert lone == [("timer", us(195).ps), ("parked", us(205).ps), ("placed", us(215).ps)]
    for third in (20, 5, 1):
        assert timer_at_195(u575, third) == lone


def fired_by_a_time_pause(u575: popcorn.PartDescription, third: int) -> tuple[list[int], list[int]]:
    fired: list[int] = []
    a = member(u575, "a", code=nop_loop(40))
    b = member(u575, "b", code=nop_loop(40))
    for due in (180_000_000, 190_000_000, 195_000_000, 198_000_000, 199_000_000, 199_500_000):
        a.loop.call_at(due / PS_PER_SECOND, lambda: fired.append(stamp(a.loop)))
    with System({"a": a, "b": b}, links(third)) as system:
        asyncio.run(system.run(for_=us(200)))
        at_the_pause = list(fired)
        asyncio.run(system.run(for_=us(100)))
    return at_the_pause, fired


def test_what_fired_by_a_time_pause_follows_the_quantum_and_the_stamps_hold(
    u575: popcorn.PartDescription,
) -> None:
    every = [180_000_000, 190_000_000, 195_000_000, 198_000_000, 199_000_000, 199_500_000]
    assert fired_by_a_time_pause(u575, 20) == (every, every)
    assert fired_by_a_time_pause(u575, 1) == (every[:2], every)


def test_after_an_error_on_a_member_the_other_stands_where_the_quantum_left_it(
    u575: popcorn.PartDescription,
) -> None:
    def clock_of_a(third: int) -> int:
        a = member(u575, "a", code=nop_loop(40))
        b = member(u575, "b", code=nop_loop(40))

        def raises() -> None:
            raise LookupError("no such entry")

        b.loop.call_at(100e-6, raises)
        with System({"a": a, "b": b}, links(third)) as system:
            with pytest.raises(popcorn.ModelError):
                asyncio.run(system.run(for_=ms(1)))
            return a.time.ps

    assert [clock_of_a(third) for third in (20, 5, 1)] == [123_000_000, 112_750_000, 102_500_000]


def forwarder(u575: popcorn.PartDescription, m: popcorn.Machine, x: str, y: str) -> bytes:
    """Polls UART `x` and writes what it holds to UART `y`, and the other way round."""
    bases = [next(p.base for p in u575.peripherals if p.name == name) for name in (x, y)]
    assert m.periph[x].ISR_enabled.offset == 0x1C
    assert m.periph[x].RDR.offset == 0x24
    assert m.periph[x].TDR.offset == 0x28
    words = (0x4806, 0x4907)
    words += (0x69C2, 0x0692, 0xD501, 0x6A43, 0x628B)
    words += (0x69CA, 0x0692, 0xD501, 0x6A4B, 0x6283)
    words += (0xE7F4, 0x0000)
    code = b"".join(word.to_bytes(2, "little") for word in words)
    return code + b"".join(base.to_bytes(4, "little") for base in bases)


def traffic_scene(
    u575: popcorn.PartDescription,
    third: int | None,
    *,
    slice_instructions: int = 1_000_000,
    goals: Sequence[int] = (3_000_000_000,),
) -> tuple[list[tuple[object, ...]], list[int]]:
    """A device's timer sends on a's USART1, a's firmware forwards it to its linked USART2, b echoes
    it, a forwards it back, the device hears it and arms a timer. Each callback logs `Machine.time`
    with its stamps. Returns that log and the arrivals on the link."""
    log: list[tuple[object, ...]] = []
    state = [0]

    class Peer(UartDevice):
        def on_attach(self) -> None:
            self.loop.call_later(1e-5, self.tick)

        def tick(self) -> None:
            log.append(("tick", state[0], self.now.ps, stamp(self.loop), a.time.ps))
            if state[0] < 6:
                self.send(bytes([0x40 + state[0]]))
                state[0] += 1
                self.loop.call_later(3.3e-4, self.tick)

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            log.append(("frame", byte, at.ps, self.now.ps, a.time.ps))
            self.loop.call_later(1.7e-5, self.late, byte)

        def late(self, byte: int) -> None:
            log.append(("late", byte, self.now.ps, stamp(self.loop), a.time.ps))

    def made(name: str, devices: Mapping[str, Sequence[UartDevice]] | None) -> popcorn.Machine:
        m = popcorn.Machine(u575, devices=devices, name=name, slice_instructions=slice_instructions)
        m.reset()
        parked_on(m)
        for uart in ("USART1", "USART2", "USART3"):
            uart_on(m, uart)
        return m

    a = made("a", {"USART1": [Peer()]})
    b = made("b", None)
    a.mem.write(RAM, forwarder(u575, a, "USART1", "USART2"))
    b.mem.write(RAM, forwarder(u575, b, "USART2", "USART2"))
    with System({"a": a, "b": b}, links(third), slice_instructions=slice_instructions) as system:
        tap = system.tap("a.USART2")
        for goal in goals:
            asyncio.run(system.run(for_=ps(goal)))
        return log, [frame.arrival.ps for frame in tap.frames()]


def test_traffic_through_a_link_reaches_the_device_the_same_at_every_quantum(
    u575: popcorn.PartDescription,
) -> None:
    first = traffic_scene(u575, None)
    log, arrivals = first
    assert len(arrivals) == 12
    assert [entry[1] for entry in log if entry[0] == "frame"] == [0x40 + n for n in range(6)]
    assert any(entry[-1] != entry[2] for entry in log)
    assert traffic_scene(u575, 7) == first
    assert traffic_scene(u575, 5) == first
    assert traffic_scene(u575, 1) == first
    assert traffic_scene(u575, 5, slice_instructions=7_777) == first
    assert traffic_scene(u575, None, goals=[37_000_000] * 81) == first
    assert traffic_scene(u575, 1, goals=[37_000_000] * 81) == first


def test_traffic_through_a_link_is_the_same_at_every_cpu_count(
    u575: popcorn.PartDescription,
) -> None:
    cpus = sorted(os.sched_getaffinity(0))
    first = traffic_scene(u575, 5)
    try:
        os.sched_setaffinity(0, cpus[:1])
        assert traffic_scene(u575, 5) == first
        os.sched_setaffinity(0, cpus[:2])
        assert traffic_scene(u575, 5) == first
    finally:
        os.sched_setaffinity(0, cpus)


class Resetting(Stack):
    """A timer before the reset, and a `call_later` and a `call_soon` from its hook."""

    def __init__(self) -> None:
        super().__init__(lambda device: device.loop.call_later(1.5e-4, device.mark, "kept"))

    def reset(self, kind: popcorn.ResetKind) -> None:
        self.log.append((kind.name, stamp(self.loop), self.now.ps))
        self.loop.call_later(1e-5, self.mark, "later")
        self.loop.call_soon(self.mark, "soon")


def after_a_reset(
    u575: popcorn.PartDescription,
    kind: popcorn.ResetKind,
    third: int | None,
    reset: Callable[[System, popcorn.Machine], object] | None,
) -> Log:
    device = Resetting()
    a = member(u575, "a", {"USART1": [device]})
    if third is None:
        asyncio.run(a.run(for_=us(100)))
        device.log.clear()
        a.reset(kind)
        parked_on(a)
        asyncio.run(a.run(for_=us(100)))
        return device.log
    assert reset is not None
    with System({"a": a, "b": member(u575, "b")}, links(third)) as system:
        asyncio.run(system.run(for_=us(100)))
        device.log.clear()
        reset(system, a)
        parked_on(a)
        asyncio.run(system.run(for_=us(100)))
    return device.log


@pytest.mark.parametrize(
    "kind", [popcorn.ResetKind.SOFTWARE, popcorn.ResetKind.PIN, popcorn.ResetKind.POWER_ON]
)
def test_a_reset_of_a_member_with_a_device_runs_its_hook_in_the_loop(
    u575: popcorn.PartDescription, kind: popcorn.ResetKind
) -> None:
    lone = after_a_reset(u575, kind, None, None)
    assert lone == [
        (kind.name, us(100).ps, 0),
        ("soon", us(100).ps, 0),
        ("later", us(110).ps, us(10).ps),
        ("kept", us(150).ps, us(50).ps),
    ]
    for third in (20, 5, 1):
        by_system = after_a_reset(u575, kind, third, lambda system, _: system.reset("a", kind))
        assert by_system == lone
        by_member = after_a_reset(u575, kind, third, lambda _, a: a.reset(kind))
        assert by_member == lone


def timer_then_reset(u575: popcorn.PartDescription, third: int) -> tuple[list[int], Log]:
    device = Stack()
    a = member(u575, "a", {"USART1": [device]}, nop_loop(40))
    device.loop.call_at(195e-6, device.mark, "timer")
    with System({"a": a, "b": member(u575, "b", code=nop_loop(40))}, links(third)) as system:
        asyncio.run(system.run(for_=us(200)))
        fired = [at for _, at, _ in device.log]
        system.reset("a", popcorn.ResetKind.SOFTWARE)
        parked_on(a)
        asyncio.run(system.run(for_=us(100)))
    return fired, device.log


def test_a_member_reset_at_a_time_pause_moves_a_timer_the_quantum_held(
    u575: popcorn.PartDescription,
) -> None:
    fired = [("timer", us(195).ps, us(195).ps)]
    assert timer_then_reset(u575, 20) == ([us(195).ps], fired)
    moved = [("timer", us(205).ps, 0)]
    assert timer_then_reset(u575, 5) == ([], moved)
    assert timer_then_reset(u575, 1) == ([], moved)


def frame_then_reset(
    u575: popcorn.PartDescription, third: int, *, device: bool
) -> tuple[int | None, list[int]]:
    """A frame due at 197.75 us, a TIME pause at 200 us with the clock at 205 us, a member reset."""
    heard = Stack()
    a = member(u575, "a", {"USART1": [heard]} if device else None, nop_loop(40))
    a.periph.USART1.CR1_enabled.UE = 0
    a.periph.USART1.BRR.value = BRR - 1
    a.periph.USART1.CR1_disabled.UE = 1
    with System({"a": a, "b": member(u575, "b", code=nop_loop(40))}, links(third)) as system:
        port = None if device else a.periph.USART1.port
        asyncio.run(system.run(for_=us(107)))
        a.periph.USART1.TDR.value = 0x41
        asyncio.run(system.run(for_=us(93)))
        system.reset("a", popcorn.ResetKind.SOFTWARE)
        parked_on(a)
        asyncio.run(system.run(for_=us(100)))
        return (None if port is None else port.available), [at for _, at, _ in heard.log]


def test_a_member_reset_at_a_time_pause_drops_a_frame_the_quantum_held(
    u575: popcorn.PartDescription,
) -> None:
    assert frame_then_reset(u575, 20, device=True) == (None, [197_750_000])
    assert frame_then_reset(u575, 5, device=True) == (None, [])
    assert frame_then_reset(u575, 20, device=False) == (1, [])
    assert frame_then_reset(u575, 5, device=False) == (0, [])


def ticking(device: Stack) -> None:
    def tick() -> None:
        device.mark("tick")
        device.loop.call_later(1e-4, tick)

    device.loop.call_later(1e-4, tick)


def ops_keep_the_loop(
    u575: popcorn.PartDescription,
    third: int,
    operation: Callable[[System, popcorn.Machine], object],
) -> Log:
    device = Stack(ticking)
    a = member(u575, "a", {"USART1": [device]}, nop_loop(40))
    with System({"a": a, "b": member(u575, "b", code=nop_loop(40))}, links(third)) as system:
        asyncio.run(system.run(for_=us(150)))
        device.log.clear()
        operation(system, a)
        asyncio.run(system.run(for_=us(300)))
    return device.log


def member_running(system: System, _: popcorn.Machine) -> None:
    async def body() -> None:
        async with system.running():
            await asyncio.sleep(0)

    asyncio.run(body())


def member_start_halt(system: System, _: popcorn.Machine) -> None:
    async def body() -> None:
        system.start()
        system.halt()

    asyncio.run(body())


def member_step(_: System, a: popcorn.Machine) -> None:
    asyncio.run(a.step())


def member_load(_: System, a: popcorn.Machine) -> None:
    a.load(popcorn.Image.elf(BOARD_ELF))


def member_mem(_: System, a: popcorn.Machine) -> None:
    assert a.mem.u32[RAM] != 0


def member_remove_hooks(_: System, a: popcorn.Machine) -> None:
    a.remove_hooks()


@pytest.mark.parametrize(
    "operation",
    [
        member_step,
        member_load,
        member_mem,
        member_remove_hooks,
        member_running,
        member_start_halt,
    ],
)
def test_an_operation_on_a_member_with_a_device_keeps_its_loop(
    u575: popcorn.PartDescription, operation: Callable[[System, popcorn.Machine], object]
) -> None:
    log = ops_keep_the_loop(u575, 5, operation)
    assert [name for name, _, _ in log[:2]] == ["tick", "tick"]
    assert [at for _, at, _ in log[:2]] == [us(200).ps, us(300).ps]
    assert all(at == now for _, at, now in log)
    assert ops_keep_the_loop(u575, 1, operation) == log


def test_a_member_with_a_device_refuses_a_power_cycle_and_its_own_snapshot(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "a", {"USART1": [Stack()]})
    with System({"a": a, "b": member(u575, "b")}, links(5)):
        with pytest.raises(popcorn.MachineBusy, match="no power_cycle"):
            a.power_cycle()
        with pytest.raises(popcorn.MachineBusy, match="no snapshot"):
            a.snapshot()
