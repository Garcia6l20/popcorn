"""The restore rule of a machine's loop: refusals, emptying, native restore, stamp, then the
device's `restore`, on a lone machine and on the members of a `System`."""

from __future__ import annotations

import asyncio
import contextvars
import gc
import weakref
from collections.abc import AsyncIterator, Callable, Coroutine, Mapping, Sequence
from typing import Any, cast

import popcorn
import pytest
from popcorn import MachineRunning, ModelError, SnapshotRefused, System, UartLink
from popcorn.arm import Reg
from popcorn.board import I2CTarget, SpiTarget, UartDevice, UartError
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import collected

RAM = 0x2000_0000
NEVER = RAM + 0x1000
THUMB = 1 << 24
BRR = 35
PS_PER_SECOND = 10**12
WFI_LOOP = b"\x30\xbf\xfd\xe7"
NOP = b"\x00\xbf"
BKPT = b"\x00\xbe"
GPIOA = 0x4202_0000
TOGGLE = bytes.fromhex(
    "0b484ff020014ff40012016000bf00bf00bf00bf00bf00bf00bf00bf026000bf00bf00bf00bf00bf00bf00bf"
    "00bfece7"
) + (GPIOA + 0x18).to_bytes(4, "little")

type Devices = Mapping[str, Sequence[UartDevice | I2CTarget | SpiTarget]]


@pytest.fixture(scope="session")
def u575() -> popcorn.PartDescription:
    from popcorn.stm32 import mcu

    return mcu("STM32U575QGIx")


def nop_loop(nops: int) -> bytes:
    branch = 0xE000 | ((-(2 * nops + 4)) >> 1 & 0x7FF)
    return NOP * nops + branch.to_bytes(2, "little")


def stamp(loop: asyncio.AbstractEventLoop) -> int:
    return round(loop.time() * PS_PER_SECOND)


def uart_on(m: popcorn.Machine, name: str = "USART1", *, rtse: bool = False) -> None:
    unit = getattr(m.periph, name)
    enable = m.periph.RCC.APB2ENR if name == "USART1" else m.periph.RCC.APB1ENR1
    setattr(enable, f"{name}EN", 1)
    unit.BRR.value = BRR
    if rtse:
        unit.CR3.RTSE = 1
    unit.CR1_disabled.poke(UE=1, TE=1, RE=1)


def asleep(
    u575: popcorn.PartDescription,
    devices: Devices | None = None,
    code: bytes = WFI_LOOP,
    *,
    name: str | None = None,
    rtse: bool = False,
) -> popcorn.Machine:
    m = popcorn.Machine(u575, devices=devices, name=name)
    m.reset()
    m.periph.RCC.AHB2ENR1.GPIOAEN = 1
    m.periph.GPIOA.MODER.MODE5 = 1
    m.periph.GPIOA.MODER.MODE0 = 0
    m.periph.GPIOA.MODER.MODE1 = 0
    m.mem.write(RAM, code)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    uart_on(m, rtse=rtse)
    return m


def run(m: popcorn.Machine, duration: VirtualDuration) -> None:
    asyncio.run(m.run(for_=duration))


class Stack(UartDevice):
    """Counts its builds, logs what it hears, and runs `build` inside `on_attach`."""

    def __init__(self, build: Callable[[Stack], object] | None = None) -> None:
        super().__init__()
        self.build = build
        self.builds = 0
        self.log: list[tuple[str, int]] = []

    def on_attach(self) -> None:
        self.builds += 1
        if self.build is not None:
            self.build(self)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.log.append((f"frame {byte:#x}", at.ps))

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.log.append((kind.name, at.ps))

    def mark(self, what: str) -> None:
        self.log.append((what, stamp(self.loop)))


class Saved(Stack):
    """A stack with its own bytes, whose `restore` rebuilds a timer from them."""

    def __init__(self, build: Callable[[Stack], object] | None = None) -> None:
        super().__init__(build)
        self.state = b"boot"
        self.restored: list[bytes] = []

    def snapshot(self) -> bytes:
        return self.state

    def restore(self, data: bytes) -> None:
        self.restored.append(data)
        self.state = data
        self.loop.call_later(1e-4, self.mark, "rebuilt")


def test_a_restore_drops_old_timers_and_a_rebuilt_one_fires(u575: popcorn.PartDescription) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    saved = m.snapshot()
    device.loop.call_later(1e-5, device.mark, "old")
    device.state = b"later"
    m.restore(saved)
    assert device.restored == [b"boot"]
    assert device.state == b"boot"
    run(m, ms(1))
    assert device.log == [("rebuilt", us(200).ps)]


def test_a_task_runs_to_its_finally_on_the_calling_thread_with_the_old_timeline(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[object] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(50))
    saved = m.snapshot()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            await asyncio.sleep(0)
            seen.append(stamp(device.loop))
            seen.append(asyncio.get_running_loop() is device.loop)
            seen.append(m.mem.u32[RAM + 0x700])

    device.loop.create_task(pending())
    run(m, us(50))
    m.mem.u32[RAM + 0x700] = 7
    m.restore(saved)
    assert seen == [us(100).ps, True, 7]
    assert m.mem.u32[RAM + 0x700] == 0


def test_a_restore_withdraws_the_armed_event(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)
    run(m, us(100))
    saved = m.snapshot()
    pending = m.pending_events
    m.loop.call_later(1, print)
    assert m.pending_events == pending + 1
    m.restore(saved)
    assert m.pending_events == pending
    assert m.loop._armed is None  # pyright: ignore[reportPrivateUsage]


def test_a_restore_gives_back_the_quiet_of_its_snapshot(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    quiet = m.snapshot()
    device.loop.call_soon(sends, device)
    run(m, us(10))
    with pytest.raises(SnapshotRefused):
        m.snapshot()
    m.restore(quiet)
    m.snapshot()


def test_a_restore_gives_back_the_traffic_of_its_snapshot(u575: popcorn.PartDescription) -> None:
    class Partial(Stack):
        def snapshot(self) -> bytes:
            return b""

    device = Partial()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    device.loop.call_soon(sends, device)
    run(m, us(10))
    talked = m.snapshot()
    m.restore(talked)
    with pytest.raises(SnapshotRefused):
        UartDevice.snapshot(device)


def test_a_rerun_of_on_attach_at_the_restored_clock(u575: popcorn.PartDescription) -> None:
    device = Stack(lambda stack: stack.mark("attach"))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    saved = m.snapshot()
    at = m.time.ps
    run(m, us(100))
    device.log.clear()
    m.restore(saved)
    assert device.builds == 1
    run(m, us(10))
    assert device.builds == 2
    assert device.log == [("attach", at)]


def test_an_override_does_not_rerun_and_super_does(u575: popcorn.PartDescription) -> None:
    class Calling(Stack):
        def restore(self, data: bytes) -> None:
            super().restore(data)

    plain = Saved()
    calling = Calling()
    m = asleep(u575, {"USART1": [plain], "USART2": [calling]})
    run(m, us(100))
    m.restore(m.snapshot())
    run(m, us(10))
    assert plain.builds == 1
    assert calling.builds == 2


def test_a_new_machine_reruns_a_default_device_and_not_an_override(
    u575: popcorn.PartDescription,
) -> None:
    first, second = Stack(), Saved()
    m = asleep(u575, {"USART1": [first], "USART2": [second]})
    run(m, us(100))
    data = m.snapshot().to_bytes()
    default, override = Stack(), Saved()
    other = popcorn.Machine(u575, devices={"USART1": [default], "USART2": [override]})
    other.restore(popcorn.Snapshot.from_bytes(data))
    run(other, us(10))
    assert default.builds == 1
    assert override.builds == 0
    assert override.restored == [b"boot"]


def test_a_snapshot_ahead_of_the_first_attach_attaches_with_its_send(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack(lambda stack: stack.send(b"A"))
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    saved = m.snapshot()
    m.reg[Reg.PC] = RAM
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    m.reg[Reg.SP] = RAM + 0x800
    run(m, us(1))
    assert device.builds == 1
    m.restore(saved)
    m.mem.write(RAM, WFI_LOOP)
    run(m, us(1))
    assert device.builds == 2


def test_a_replay_of_a_snapshot_ahead_of_the_first_attach_gives_the_same_bytes(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack(lambda stack: stack.mark("attach"))
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    saved = m.snapshot()
    run(m, us(5))
    first = without_generation(m.snapshot())
    assert device.builds == 1
    m.restore(saved)
    run(m, us(5))
    assert device.builds == 2
    assert without_generation(m.snapshot()) == first


def test_a_snapshot_with_no_device_attaches_a_device_once(u575: popcorn.PartDescription) -> None:
    data = popcorn.Machine(u575).snapshot().to_bytes()
    device = Stack()
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.restore(popcorn.Snapshot.from_bytes(data))
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.XPSR] = THUMB
    run(m, us(1))
    assert device.builds == 1


def test_a_replay_with_a_watched_pin_gives_the_same_bytes(u575: popcorn.PartDescription) -> None:
    log: list[tuple[bool, int]] = []
    device = UartDevice()
    m = asleep(u575, {"USART1": [device]}, TOGGLE)
    device.board.watch_pin("PA5", lambda level, at: log.append((level, at.ps)))
    run(m, us(6))
    saved = m.snapshot()
    held = len(log)
    run(m, us(30))
    first = without_generation(m.snapshot())
    went_on = log[held:]
    assert went_on
    del log[held:]
    m.restore(saved)
    run(m, us(30))
    assert log[held:] == went_on
    assert without_generation(m.snapshot()) == first


def sends(device: Stack) -> None:
    device.send(b"A")


def breaks(device: Stack) -> None:
    device.send_break()


def drives(device: Stack) -> None:
    device.drive_cts(False)


@pytest.mark.parametrize("act", [sends, breaks, drives])
def test_a_device_that_acted_on_the_wire_refuses_and_names_itself(
    u575: popcorn.PartDescription, act: Callable[[Stack], None]
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    device.loop.call_soon(act, device)
    run(m, us(10))
    with pytest.raises(
        SnapshotRefused, match=r"Stack on USART1.*override snapshot\(\) and restore"
    ):
        m.snapshot()
    run(m, us(10))


def test_a_frame_the_firmware_sent_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert device.log
    with pytest.raises(SnapshotRefused) as refused:
        m.snapshot()
    assert refused.value.device == repr(device)


def test_an_abort_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, us(5))
    m.snapshot()
    m.periph.USART1.CR1_enabled.UE = 0
    run(m, us(10))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_break_the_firmware_sent_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.RQR.SBKRQ = 1
    run(m, ms(1))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_snapshot_refused_changes_nothing_and_an_override_saves(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    saved = m.snapshot()
    m.restore(saved)
    assert device.restored == [b"boot"]


def test_a_quiet_boot_restored_hears_the_same_bytes(u575: popcorn.PartDescription) -> None:
    def later(stack: Stack) -> None:
        stack.loop.call_later(5e-4, stack.send, b"Z")

    device = Stack(later)
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    saved = m.snapshot()
    run(m, ms(2))
    first = m.periph.USART1.RDR.value, m.time.ps, m.periph.USART1.ISR_enabled.RXFNE
    assert m.periph.USART1.ISR_enabled.RXFNE == 1
    m.restore(saved)
    run(m, ms(2))
    assert (m.periph.USART1.RDR.value, m.time.ps, m.periph.USART1.ISR_enabled.RXFNE) == first


def test_a_talking_boot_is_refused_where_a_rerun_would_add_a_byte(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack(sends)
    m = asleep(u575, {"USART1": [device]})
    run(m, ms(1))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_the_wire_is_refused_from_a_rerun_and_open_to_a_later_timer(
    u575: popcorn.PartDescription,
) -> None:
    errors: list[str] = []
    armed = [False]

    def build(stack: Stack) -> None:
        if not armed[0]:
            armed[0] = True
            return
        stack.loop.call_later(1e-4, stack.send, b"T")
        for act in (sends, breaks, drives):
            try:
                act(stack)
            except RuntimeError as error:
                errors.append(str(error))

        async def first_step() -> None:
            await asyncio.sleep(0)
            try:
                stack.send(b"B")
            except RuntimeError as error:
                errors.append(str(error))

        stack.loop.create_task(first_step())

    device = Stack(build)
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    m.restore(m.snapshot())
    run(m, us(10))
    assert len(errors) == 4
    assert all("on_attach runs again" in error for error in errors)
    run(m, ms(1))
    assert m.periph.USART1.ISR_enabled.RXFNE == 1
    assert m.periph.USART1.RDR.value == ord("T")
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_send_from_a_rerun_raises_as_a_model_error(u575: popcorn.PartDescription) -> None:
    calls = [0]

    def build(stack: Stack) -> None:
        calls[0] += 1
        if calls[0] == 2:
            stack.send(b"A")

    m = asleep(u575, {"USART1": [Stack(build)]})
    run(m, us(100))
    m.restore(m.snapshot())
    with pytest.raises(ModelError, match="on_attach runs again"):
        run(m, us(10))


def test_the_wire_is_refused_inside_restore(u575: popcorn.PartDescription) -> None:
    class Talking(Saved):
        def restore(self, data: bytes) -> None:
            self.send(b"A")

    m = asleep(u575, {"USART1": [Talking()]})
    run(m, us(100))
    saved = m.snapshot()
    with pytest.raises(ModelError, match=r"refused inside restore\(\)"):
        m.restore(saved)
    run(m, us(10))
    assert m.periph.USART1.ISR_enabled.RXFNE == 0


def test_a_call_later_in_restore_counts_from_a_clock_behind_the_old_one(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(100))
    saved = m.snapshot()
    run(m, ms(5))
    m.restore(saved)
    run(m, ms(1))
    assert device.log == [("rebuilt", us(200).ps)]


def test_a_native_refusal_leaves_the_loop_as_it_was(u575: popcorn.PartDescription) -> None:
    from popcorn.stm32 import mcu

    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    device.loop.call_later(1e-5, device.mark, "kept")
    other = popcorn.Machine(mcu("STM32H573AIIx"))
    foreign = popcorn.Snapshot.from_bytes(other.snapshot().to_bytes())
    with pytest.raises(popcorn.ConfigError):
        m.restore(foreign)
    with pytest.raises(popcorn.ConfigError):
        m.restore(other.snapshot())
    run(m, us(50))
    assert device.log == [("kept", us(10).ps)]


def failing_finally(loop: asyncio.AbstractEventLoop, message: str) -> None:
    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            raise LookupError(message)

    loop.create_task(pending())


def test_a_finally_error_raises_once_the_restore_has_finished(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(50))
    saved = m.snapshot()
    failing_finally(device.loop, "first")
    failing_finally(device.loop, "second")
    run(m, us(50))
    with pytest.raises(ModelError, match="LookupError\\('first'\\)") as raised:
        m.restore(saved)
    assert "second" in " ".join(raised.value.__notes__)
    assert m.time.ps == us(50).ps
    assert device.restored == [b"boot"]
    run(m, ms(1))
    assert device.log == [("rebuilt", us(150).ps)]


def test_a_send_from_a_finally_is_refused(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(50))
    saved = m.snapshot()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            device.send(b"A")

    device.loop.create_task(pending())
    run(m, us(10))
    with pytest.raises(ModelError, match="refused while a restore empties"):
        m.restore(saved)
    run(m, us(10))
    assert m.periph.USART1.ISR_enabled.RXFNE == 0


def test_a_restore_snapshot_or_run_from_a_finally_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(50))
    saved = m.snapshot()
    seen: list[str] = []

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            for what in (lambda: m.restore(saved), m.snapshot, m.remove_hooks):
                try:
                    what()
                except MachineRunning as error:
                    seen.append(type(error).__name__)

    device.loop.create_task(pending())
    run(m, us(10))
    m.restore(saved)
    assert seen == ["MachineRunning"] * 3
    assert m.time.ps == us(50).ps


def test_a_run_from_a_finally_is_refused(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(50))
    saved = m.snapshot()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            await m.run(for_=us(1))

    device.loop.create_task(pending())
    run(m, us(10))
    with pytest.raises(ModelError, match="MachineRunning"):
        m.restore(saved)
    assert m.time.ps == us(50).ps
    run(m, us(10))
    assert m.time.ps == us(60).ps


def test_a_restore_from_a_callback_or_a_hook_raises_machine_running(
    u575: popcorn.PartDescription,
) -> None:
    caught: list[type[BaseException]] = []

    class Restoring(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            if saved:
                self.attempt()

        def attempt(self) -> None:
            try:
                m.restore(saved[0])
            except MachineRunning as error:
                caught.append(type(error))

    device = Restoring()
    saved: list[popcorn.Snapshot] = []
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved.append(m.snapshot())
    device.loop.call_soon(device.attempt)
    run(m, us(10))
    assert caught == [MachineRunning]
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert caught == [MachineRunning, MachineRunning]


def test_what_a_reset_hook_left_ready_goes(u575: popcorn.PartDescription) -> None:
    ran: list[str] = []

    async def never() -> None:
        ran.append("task")

    class Readying(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            if armed:
                self.loop.call_soon(ran.append, "callback")
                self.loop.create_task(never())

    armed: list[bool] = []
    device = Readying()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()
    armed.append(True)
    m.reset(popcorn.ResetKind.SOFTWARE)
    armed.clear()
    m.restore(saved)
    run(m, us(10))
    gc.collect()
    assert ran == []


def test_a_task_woken_by_a_reset_hook_still_runs_its_finally(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[str] = []

    class Waking(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            if held and not held[0].done():
                held[0].set_result(None)

    held: list[asyncio.Future[None]] = []
    device = Waking()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()

    async def waits() -> None:
        try:
            await held[0]
            await asyncio.sleep(3600)
        finally:
            await asyncio.sleep(0)
            seen.append("finally")

    held.append(device.loop.create_future())
    device.loop.create_task(waits())
    run(m, us(1))
    m.reset(popcorn.ResetKind.SOFTWARE)
    m.restore(saved)
    assert seen == ["finally"]


def test_a_parked_call_goes_with_the_timers(u575: popcorn.PartDescription) -> None:
    ran: list[str] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()
    device.loop.call_soon(ran.append, "parked")
    m.restore(saved)
    run(m, us(10))
    assert ran == []


def test_an_async_generator_left_unfinished_closes(u575: popcorn.PartDescription) -> None:
    seen: list[str] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()

    async def numbers() -> AsyncIterator[int]:
        try:
            yield 1
            yield 2
        finally:
            seen.append("closed")

    held: list[object] = []

    async def first() -> None:
        generator = numbers()
        held.append(generator)
        await anext(generator)

    device.loop.create_task(first())
    run(m, us(10))
    m.restore(saved)
    assert seen == ["closed"]


def test_an_await_pending_past_the_drain_is_dropped_and_stays_dropped(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    seen: list[str] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()
    held = device.loop.create_future()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            seen.append("entered")
            await held
            seen.append("resumed")

    task = device.loop.create_task(pending())
    run(m, us(10))
    m.restore(saved)
    assert seen == ["entered"]
    device.loop.call_soon(held.set_result, 1)
    run(m, us(10))
    assert seen == ["entered"]
    assert not caplog.records
    del task


def test_the_callback_bound_holds_in_the_drain(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()

    async def spinning() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            while True:
                await asyncio.sleep(0)

    device.loop.create_task(spinning())
    run(m, us(10))
    with pytest.raises(ModelError, match="callbacks at one stamp"):
        m.restore(saved)
    assert m.time.ps == us(10).ps
    run(m, us(10))
    assert m.time.ps == us(20).ps


def test_a_handler_and_a_factory_end_at_a_restore(u575: popcorn.PartDescription) -> None:
    heard: list[object] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()

    def handler(_: asyncio.AbstractEventLoop, context: dict[str, Any]) -> None:
        heard.append(context["exception"])

    def factory(
        loop: asyncio.AbstractEventLoop, coro: Coroutine[Any, Any, Any], **kwargs: Any
    ) -> asyncio.Task[Any]:
        return asyncio.Task(coro, loop=loop, **kwargs)

    device.loop.set_exception_handler(handler)
    device.loop.set_task_factory(factory)
    failing_finally(device.loop, "told")
    run(m, us(10))
    m.restore(saved)
    assert [str(error) for error in heard] == ["told"]
    assert device.loop.get_exception_handler() is None
    assert device.loop.get_task_factory() is None


def test_a_close_from_a_finally_takes_effect_and_the_restore_finishes(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            device.loop.close()

    device.loop.create_task(pending())
    run(m, us(10))
    m.restore(saved)
    assert device.loop.is_closed()
    assert m.time.ps == us(10).ps
    assert device.builds == 1
    run(m, us(10))
    assert device.builds == 1


def test_a_restore_keeps_no_machine_or_task_alive(u575: popcorn.PartDescription) -> None:
    class Held:
        pass

    captured = [Held()]
    probe = weakref.ref(captured[0])

    def body() -> weakref.ref[popcorn.Machine]:
        device = Saved()
        m = asleep(u575, {"USART1": [device]})
        run(m, us(10))
        saved = m.snapshot()

        async def pending(held: object) -> None:
            await asyncio.sleep(3600)
            assert held

        device.loop.create_task(pending(captured.pop()))
        device.loop.call_later(1, print, "never")
        run(m, us(10))
        m.restore(saved)
        run(m, us(10))
        device.loop.close()
        return weakref.ref(m)

    machine = body()
    assert collected(machine)
    assert collected(probe)


def test_a_thousand_restores_hold_no_more_than_one(u575: popcorn.PartDescription) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    saved = m.snapshot()
    loop = device.loop
    held = loop.create_future()

    async def waits() -> None:
        try:
            await held
        finally:
            await held

    for _ in range(1000):
        loop.create_task(waits())
        m.restore(saved)
    assert len(loop._timers) == 1  # pyright: ignore[reportPrivateUsage]
    assert not loop._tasks  # pyright: ignore[reportPrivateUsage]
    assert m.pending_events < 10
    assert not loop._at_fire  # pyright: ignore[reportPrivateUsage]
    gc.collect()
    assert len(loop._dropped) <= 1000  # pyright: ignore[reportPrivateUsage]


def test_a_default_device_with_no_on_attach_costs_no_event_or_gil_entry(
    u575: popcorn.PartDescription,
) -> None:
    m = asleep(u575, {"USART1": [UartDevice()]})
    run(m, us(10))
    saved = m.snapshot()
    pending = m.pending_events
    m.restore(saved)
    assert m.pending_events == pending
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    run(m, us(10))
    assert m._handle.gil_entries() == before  # pyright: ignore[reportPrivateUsage]


def without_generation(snapshot: popcorn.Snapshot) -> bytes:
    """The saved bytes with the machine's restore count blanked, the one field a restore moves."""
    data = snapshot.to_bytes()
    at = data.index(b"generation\0") + len(b"generation\0")
    return data[:at] + bytes(8) + data[at + 8 :]


def test_a_replay_gives_the_bytes_of_the_first_run(u575: popcorn.PartDescription) -> None:
    class Timed(Stack):
        def snapshot(self) -> bytes:
            return b""

        def restore(self, data: bytes) -> None:
            self.loop.call_at(1e-4, self.mark, "timer")

    device = Timed(lambda stack: stack.loop.call_at(1e-4, stack.mark, "timer"))
    m = asleep(u575, {"USART1": [device]}, nop_loop(40))
    run(m, us(50))
    saved = m.snapshot()
    run(m, us(30))
    m.mem.u32[RAM + 0x400] = 0
    run(m, ms(1))
    first = without_generation(m.snapshot())
    log = list(device.log)
    device.log.clear()
    m.restore(saved)
    run(m, us(30))
    m.mem.u32[RAM + 0x400] = 0
    run(m, ms(1))
    assert device.log == log
    assert without_generation(m.snapshot()) == first


def test_a_timer_leaves_every_sequence_counter_alone(u575: popcorn.PartDescription) -> None:
    def saved(timers: int) -> bytes:
        m = asleep(u575)
        for index in range(timers):
            m.loop.call_later((index + 1) * 1e-6, lambda: None)
        run(m, us(100))
        return m.snapshot().to_bytes()

    assert saved(5) == saved(0)


def test_a_timer_fires_behind_a_host_lane_event_of_its_count(
    u575: popcorn.PartDescription,
) -> None:
    order: list[str] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]}, nop_loop(40))
    device.loop.call_later(1e-5, lambda: order.append("timer"))
    device.board.watch_pin("PA0", lambda level, at: order.append("edge"))
    m.periph.GPIOA.pin(0).drive_at(True, at=VirtualTime(us(10).ps))
    run(m, us(50))
    assert order == ["edge", "timer"]


def test_a_parked_abort_in_a_snapshot_replays_on_error(u575: popcorn.PartDescription) -> None:
    device = Saved()
    m = asleep(u575, {"USART1": [device]})
    m.periph.USART1.TDR.value = 0x41
    run(m, us(5))
    m.periph.USART1.CR1_enabled.UE = 0
    written = m.time.ps
    saved = m.snapshot()
    run(m, us(50))
    assert ("ABORTED", written) in device.log
    device.log.clear()
    m.restore(saved)
    run(m, us(50))
    assert ("ABORTED", written) in device.log
    device.log.clear()
    m.restore(saved)
    m.reset(popcorn.ResetKind.SOFTWARE)
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    run(m, us(50))
    assert not [entry for entry in device.log if entry[0] == "ABORTED"]


def test_one_device_runs_every_hook_in_one_life(u575: popcorn.PartDescription) -> None:
    class Everything(Stack):
        def on_break(self, at: VirtualTime) -> None:
            self.log.append(("break", at.ps))

        def on_rts(self, ready: bool, at: VirtualTime) -> None:
            self.log.append(("rts", at.ps))

        def snapshot(self) -> bytes:
            return b"x"

        def restore(self, data: bytes) -> None:
            self.log.append(("restore", 0))

    device = Everything(lambda stack: stack.send(b"AB", obey_rts=False))
    m = asleep(u575, {"USART1": [device]}, rtse=True)
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    m.periph.USART1.RQR.SBKRQ = 1
    run(m, ms(1))
    m.periph.USART1.TDR.value = 0x42
    run(m, us(5))
    m.periph.USART1.CR1_enabled.UE = 0
    run(m, us(5))
    m.restore(m.snapshot())
    m.reset(popcorn.ResetKind.SOFTWARE)
    names = {name.split()[0] for name, _ in device.log}
    assert {"frame", "break", "ABORTED", "restore", "rts"} <= names


def test_a_watch_stays_across_a_restore_with_no_edge_for_the_level(
    u575: popcorn.PartDescription,
) -> None:
    log: list[tuple[bool, int]] = []
    device = UartDevice()
    m = asleep(u575, {"USART1": [device]})
    watch = device.board.watch_pin("PA0", lambda level, at: log.append((level, at.ps)))
    run(m, us(2))
    saved = m.snapshot()
    m.periph.GPIOA.pin(0).level = True
    run(m, us(3))
    assert log == [(True, us(2).ps)]
    log.clear()
    m.restore(saved)
    assert watch.active
    assert device.board.pin_level("PA0") is False
    run(m, us(3))
    assert log == []
    m.periph.GPIOA.pin(0).level = True
    run(m, us(3))
    assert log == [(True, us(5).ps)]


def test_a_rerun_makes_its_watch_again_one_call_per_edge(u575: popcorn.PartDescription) -> None:
    log: list[bool] = []
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: log.append(level)))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(2))
    saved = m.snapshot()
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    size = len(table)
    for _ in range(3):
        m.restore(saved)
        run(m, us(1))
    assert len(table) == size
    m.periph.GPIOA.pin(0).level = True
    run(m, us(1))
    assert log == [True]


def test_a_watch_made_from_the_test_stays_through_a_rerun(u575: popcorn.PartDescription) -> None:
    log: list[bool] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    watch = device.board.watch_pin("PA0", lambda level, at: log.append(level))
    run(m, us(2))
    m.restore(m.snapshot())
    run(m, us(1))
    assert device.builds == 2
    assert watch.active
    m.periph.GPIOA.pin(0).level = True
    run(m, us(1))
    assert log == [True]


def test_an_override_keeps_the_watch_it_made(u575: popcorn.PartDescription) -> None:
    log: list[bool] = []
    device = Saved(lambda stack: stack.board.watch_pin("PA0", lambda level, at: log.append(level)))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(2))
    m.restore(m.snapshot())
    m.periph.GPIOA.pin(0).level = True
    run(m, us(1))
    assert log == [True]


def toggling_member(
    u575: popcorn.PartDescription, third: int
) -> tuple[System, popcorn.Machine, list[tuple[bool, int]]]:
    device = UartDevice()
    a = asleep(u575, {"USART1": [device]}, TOGGLE, name="a")
    b = asleep(u575, code=nop_loop(40), name="b")
    uart_on(a, "USART2")
    uart_on(a, "USART3")
    uart_on(b, "USART2")
    uart_on(b, "USART3")
    log: list[tuple[bool, int]] = []
    device.board.watch_pin("PA5", lambda level, at: log.append((level, at.ps)))
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(third), baud=114_285),
    ]
    return System({"a": a, "b": b}, links), a, log


@pytest.mark.parametrize("third", [20, 5])
def test_an_edge_pending_in_a_member_snapshot_is_heard_as_in_the_run_that_went_on(
    u575: popcorn.PartDescription, third: int
) -> None:
    system, _, log = toggling_member(u575, third)
    with system:
        asyncio.run(system.run(for_=VirtualDuration(5_500_000)))
        saved = system.snapshot()
        held = len(log)
        asyncio.run(system.run(for_=us(10)))
        went_on = log[held:]
        assert went_on
        del log[held:]
        system.restore(saved)
        asyncio.run(system.run(for_=us(10)))
        assert log[held:] == went_on


def member_pair(
    u575: popcorn.PartDescription, third: int, devices: Devices
) -> tuple[System, popcorn.Machine, popcorn.Machine]:
    a = asleep(u575, devices, name="a")
    b = asleep(u575, code=nop_loop(40), name="b")
    uart_on(a, "USART2")
    uart_on(a, "USART3")
    uart_on(b, "USART2")
    uart_on(b, "USART3")
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(third), baud=114_285),
    ]
    return System({"a": a, "b": b}, links), a, b


def test_a_finally_error_at_a_system_restore_raises_once_with_no_rollback(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved()
    system, a, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        at = a.time.ps
        failing_finally(a.loop, "from the old timeline")
        asyncio.run(system.run(for_=us(20)))
        with pytest.raises(ModelError, match="from the old timeline"):
            system.restore(saved)
        assert a.time.ps == at
        assert device.restored == [b"boot"]
        asyncio.run(system.run(for_=us(150)))
        assert device.log == [("rebuilt", at + us(100).ps)]


def test_a_refusing_device_fails_a_system_restore_ahead_of_any_change(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    system, a, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        device.loop.call_later(1e-6, sends, device)
        device.loop.call_later(1, device.mark, "kept")
        asyncio.run(system.run(for_=us(20)))
        at = a.time.ps
        with pytest.raises(SnapshotRefused, match="Stack on USART1"):
            system.restore(saved)
        with pytest.raises(SnapshotRefused, match="Stack on USART1"):
            system.snapshot()
        assert a.time.ps == at
        assert a.loop._armed is not None  # pyright: ignore[reportPrivateUsage]


def test_a_refusal_at_a_system_restore_leaves_a_member_stop_undelivered(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    system, _, b = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        device.loop.call_soon(sends, device)
        b.mem.write(RAM, BKPT)
        b.reg[Reg.PC] = RAM
        stop = asyncio.run(system.run(for_=us(20)))
        assert stop.member == "b"
        undecided = dict(system._undecided)  # pyright: ignore[reportPrivateUsage]
        assert undecided
        with pytest.raises(SnapshotRefused):
            system.restore(saved)
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]


def test_a_native_refusal_of_a_member_leaves_every_loop(u575: popcorn.PartDescription) -> None:
    device = Saved()
    system, a, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        taken = system.snapshot()
        members = dict(taken.members)
        members["a"] = popcorn.Machine(u575).snapshot()
        foreign = type(taken)(members, taken.state, taken.target)
        device.loop.call_later(1, device.mark, "kept")
        with pytest.raises(popcorn.ConfigError):
            system.restore(foreign)
        assert a.loop._armed is not None  # pyright: ignore[reportPrivateUsage]


def test_a_hook_on_a_member_counts_from_the_restored_clock_after_restoring_backwards(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved()
    system, a, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        at = a.time.ps
        asyncio.run(system.run(for_=ms(1)))
        system.restore(saved)
        asyncio.run(system.run(for_=us(150)))
        assert device.log == [("rebuilt", at + us(100).ps)]


def test_an_override_snapshot_reads_the_rts_of_a_member(u575: popcorn.PartDescription) -> None:
    class Reading(Saved):
        def snapshot(self) -> bytes:
            return b"\x01" if self.rts else b"\x00"

    device = Reading()
    system, _, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        system.restore(system.snapshot())
        assert device.restored == [b"\x01"]


def member_replay(
    u575: popcorn.PartDescription, third: int
) -> tuple[list[tuple[str, int]], bytes, bytes]:
    """A replay from a time pause with an edge held, with a stack rebuilt, at one quantum."""

    class Rebuilding(Saved):
        def restore(self, data: bytes) -> None:
            self.loop.call_later(3e-5, self.mark, "timer")

    device = Rebuilding()
    a = asleep(u575, {"USART1": [device]}, TOGGLE, name="a")
    b = asleep(u575, code=nop_loop(40), name="b")
    for member in (a, b):
        uart_on(member, "USART2")
        uart_on(member, "USART3")
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(third), baud=114_285),
    ]
    device.board.watch_pin("PA5", lambda level, at: device.log.append((f"{level}", at.ps)))
    with System({"a": a, "b": b}, links) as system:
        asyncio.run(system.run(for_=VirtualDuration(5_500_000)))
        saved = system.snapshot()
        asyncio.run(system.run(for_=us(100)))
        device.log.clear()
        system.restore(saved)
        asyncio.run(system.run(for_=us(200)))
        taken = system.snapshot()
        return (
            list(device.log),
            without_generation(taken.members["a"]),
            without_generation(taken.members["b"]),
        )


@pytest.mark.parametrize("third", [20, 5, 1])
def test_a_member_replay_repeats_at_one_quantum(u575: popcorn.PartDescription, third: int) -> None:
    assert member_replay(u575, third) == member_replay(u575, third)


def idle_replay(u575: popcorn.PartDescription, third: int) -> list[tuple[str, int]]:
    """A stack rebuilt from an IDLE pause, both cores asleep, run on to the next IDLE."""

    class Rebuilding(Saved):
        def restore(self, data: bytes) -> None:
            self.loop.call_later(3e-5, self.mark, "timer")

    device = Rebuilding()
    a = asleep(u575, {"USART1": [device]}, name="a")
    b = asleep(u575, name="b")
    for member in (a, b):
        uart_on(member, "USART2")
        uart_on(member, "USART3")
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(third), baud=114_285),
    ]
    with System({"a": a, "b": b}, links) as system:

        async def goals() -> tuple[popcorn.Stop, popcorn.Stop]:
            return await asyncio.gather(a.run(until=NEVER), b.run(until=NEVER))

        assert {stop.reason for stop in asyncio.run(goals())} == {popcorn.StopReason.IDLE}
        saved = system.snapshot()
        asyncio.run(system.run(for_=ms(1)))
        system.restore(saved)
        asyncio.run(system.run(for_=ms(1)))
        return list(device.log)


def test_a_member_restored_from_an_idle_pause_is_equal_at_every_quantum(
    u575: popcorn.PartDescription,
) -> None:
    runs = [idle_replay(u575, third) for third in (20, 5, 1)]
    assert runs[0] == runs[1] == runs[2]
    assert [name for name, _ in runs[0]] == ["timer"]


def toggling_stack(
    u575: popcorn.PartDescription, third: int, watches: int
) -> tuple[System, list[tuple[int, bool, int]]]:
    log: list[tuple[int, bool, int]] = []

    def build(stack: Stack) -> None:
        for index in range(watches):

            def heard(level: bool, at: VirtualTime, which: int = index) -> None:
                log.append((which, level, at.ps))

            stack.board.watch_pin("PA5", heard)

    a = asleep(u575, {"USART1": [Stack(build)]}, TOGGLE, name="a")
    b = asleep(u575, code=nop_loop(40), name="b")
    for member in (a, b):
        uart_on(member, "USART2")
        uart_on(member, "USART3")
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(third), baud=114_285),
    ]
    return System({"a": a, "b": b}, links), log


@pytest.mark.parametrize("watches", [1, 2])
@pytest.mark.parametrize("third", [20, 5])
def test_a_rerun_comes_after_the_edges_pending_at_the_restored_clock(
    u575: popcorn.PartDescription, third: int, watches: int
) -> None:
    def heard(restoring: bool) -> list[tuple[int, bool, int]]:
        system, log = toggling_stack(u575, third, watches)
        with system:
            asyncio.run(system.run(for_=VirtualDuration(5_500_000)))
            saved = system.snapshot()
            assert log == []
            if restoring:
                system.restore(saved)
            asyncio.run(system.run(for_=us(10)))
        return log

    went_on = heard(False)
    assert len([entry for entry in went_on if entry[2] == went_on[0][2]]) == 2 * watches
    assert heard(True) == went_on


def test_two_hooks_pending_at_the_restored_clock_run_ahead_of_the_rerun(
    u575: popcorn.PartDescription,
) -> None:
    order: list[str] = []

    class Replying(Stack):
        def on_error(self, kind: UartError, at: VirtualTime) -> None:
            order.append("abort")
            self.send(b"A")

    device = Replying(lambda stack: order.append("attach"))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    for _ in range(2):
        m.periph.USART1.TDR.value = 0x41
        m.periph.USART1.CR1_enabled.UE = 0
        m.periph.USART1.CR1_disabled.UE = 1
    saved = m.snapshot()
    order.clear()
    m.restore(saved)
    run(m, us(10))
    assert order == ["abort", "abort", "attach"]


def test_a_snapshot_ahead_of_the_first_attach_keeps_one_watch_across_restores(
    u575: popcorn.PartDescription,
) -> None:
    log: list[bool] = []
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: log.append(level)))
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    m.periph.RCC.AHB2ENR1.GPIOAEN = 1
    m.periph.GPIOA.MODER.MODE0 = 0
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    saved = m.snapshot()
    run(m, us(1))
    for _ in range(3):
        m.restore(saved)
        run(m, us(1))
    assert device.builds == 4
    m.periph.GPIOA.pin(0).level = True
    run(m, us(1))
    assert log == [True]


def test_a_drive_past_the_attach_stamp_refuses(u575: popcorn.PartDescription) -> None:
    def build(stack: Stack) -> None:
        stack.board.drive_pin("PA1", False)
        stack.loop.call_later(5e-6, stack.board.drive_pin, "PA1", True)

    device = Stack(build)
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_drive_due_past_the_attach_stamp_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack(lambda stack: stack.board.drive_pin("PA1", True, at=stack.now + us(5)))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_drive_at_the_attach_stamp_still_snapshots(u575: popcorn.PartDescription) -> None:
    device = Stack(lambda stack: stack.board.drive_pin("PA1", False))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    m.restore(m.snapshot())
    run(m, us(10))
    assert device.builds == 2
    m.snapshot()


def test_an_edge_delivered_to_a_watch_from_the_loop_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.snapshot()
    m.periph.GPIOA.pin(0).level = True
    m.snapshot()
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_an_edge_at_the_attach_stamp_does_not_count(u575: popcorn.PartDescription) -> None:
    heard: list[bool] = []

    def build(stack: Stack) -> None:
        stack.board.watch_pin("PA1", lambda level, at: heard.append(level))
        stack.board.drive_pin("PA1", True)

    device = Stack(build)
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    assert heard == [True]
    m.snapshot()


def test_an_edge_of_a_watch_from_the_test_thread_counts_nothing(
    u575: popcorn.PartDescription,
) -> None:
    log: list[bool] = []
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    device.board.watch_pin("PA0", lambda level, at: log.append(level))
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    assert log == [True]
    m.snapshot()


def test_an_override_snapshots_after_an_edge_and_its_super_refuses(
    u575: popcorn.PartDescription,
) -> None:
    device = Saved(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    m.snapshot()
    with pytest.raises(SnapshotRefused):
        UartDevice.snapshot(device)
    m.periph.GPIOA.pin(0).level = False
    quiet = Saved(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    n = asleep(u575, {"USART1": [quiet]})
    run(n, us(5))
    n.restore(n.snapshot())
    UartDevice.snapshot(quiet)
    n.periph.GPIOA.pin(0).level = True
    run(n, us(5))
    with pytest.raises(SnapshotRefused):
        UartDevice.snapshot(quiet)


@pytest.mark.parametrize("restoring", [False, True])
def test_an_edge_pending_at_the_restored_clock_counts_as_in_the_run_that_went_on(
    u575: popcorn.PartDescription, restoring: bool
) -> None:
    system, log = toggling_stack(u575, 5, 1)
    with system:
        asyncio.run(system.run(for_=VirtualDuration(5_500_000)))
        saved = system.snapshot()
        if restoring:
            system.restore(saved)
        asyncio.run(system.run(for_=us(1)))
        assert log
        with pytest.raises(SnapshotRefused):
            system.snapshot()


@pytest.mark.parametrize("restoring", [False, True])
def test_a_stack_armed_by_a_pending_edge_refuses_with_or_without_the_restore(
    u575: popcorn.PartDescription, restoring: bool
) -> None:
    def build(stack: Stack) -> None:
        def heard(level: bool, at: VirtualTime) -> None:
            stack.loop.call_later(2e-5, stack.mark, "ready")

        stack.board.watch_pin("PA0", heard)

    device = Stack(build)
    m = asleep(u575, {"USART1": [device]})
    run(m, us(2))
    m.periph.GPIOA.pin(0).level = True
    saved = m.snapshot()
    if restoring:
        m.restore(saved)
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


@pytest.mark.parametrize("restoring", [False, True])
def test_a_drive_from_a_pending_edge_refuses_with_or_without_the_restore(
    u575: popcorn.PartDescription, restoring: bool
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(2))
    device.board.watch_pin("PA0", lambda level, at: device.board.drive_pin("PA1", True))
    m.periph.GPIOA.pin(0).level = True
    saved = m.snapshot()
    if restoring:
        m.restore(saved)
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_an_edge_after_a_rerun_refuses(u575: popcorn.PartDescription) -> None:
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    assert device.builds == 2
    m.snapshot()
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_member_that_heard_an_edge_after_a_rerun_refuses_a_quiet_snapshot_too(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    system, a, _ = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        quiet = system.snapshot()
        system.restore(quiet)
        asyncio.run(system.run(for_=us(20)))
        assert device.builds == 2
        a.periph.GPIOA.pin(0).level = True
        asyncio.run(system.run(for_=us(20)))
        with pytest.raises(SnapshotRefused):
            system.snapshot()
        with pytest.raises(SnapshotRefused):
            system.restore(quiet)


def test_a_drive_off_a_step_counts_with_the_loop_still_at_the_attach_stamp(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))

    async def drives() -> None:
        async with m.running():
            await m.sample(lambda: device.board.drive_pin("PA1", True))

    asyncio.run(drives())
    with pytest.raises(SnapshotRefused):
        m.snapshot()


@pytest.mark.parametrize("order", ["ahead_of_super", "behind_super", "with_no_super"])
def test_a_drive_from_restore_counts_nothing_at_any_place_against_super(
    u575: popcorn.PartDescription, order: str
) -> None:
    class Driving(Stack):
        def restore(self, data: bytes) -> None:
            if order == "ahead_of_super":
                self.board.drive_pin("PA1", True)
            if order != "with_no_super":
                super().restore(data)
            if order != "ahead_of_super":
                self.board.drive_pin("PA1", True)

    device = Driving()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    assert device.board.pin_level("PA1")
    m.snapshot()


def test_a_drive_from_restore_due_past_the_restored_clock_counts(
    u575: popcorn.PartDescription,
) -> None:
    class Driving(Stack):
        def restore(self, data: bytes) -> None:
            self.board.drive_pin("PA1", True, at=self.now + us(1))

    m = asleep(u575, {"USART1": [Driving()]})
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_a_refused_restore_in_a_free_run_holds_no_rerun_and_a_later_edge_counts(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    builds = device.builds

    async def refused() -> None:
        async with m.running():
            with pytest.raises(MachineRunning):
                device.restore(b"")

    asyncio.run(refused())
    assert not device.loop._at_fire  # pyright: ignore[reportPrivateUsage]
    run(m, us(5))
    assert device.builds == builds
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    with pytest.raises(SnapshotRefused):
        m.snapshot()


def test_an_on_rts_delivery_counts_only_for_a_class_that_overrides_it(
    u575: popcorn.PartDescription,
) -> None:
    class Hearing(Stack):
        def on_rts(self, ready: bool, at: VirtualTime) -> None:
            self.mark(f"rts {ready}")

    def traffic_after_an_edge(device: Stack) -> bool:
        m = asleep(u575, {"USART1": [device]}, rtse=True)
        device.loop.call_soon(device.send, b"A")
        run(m, us(1))
        device._traffic = False  # pyright: ignore[reportPrivateUsage]
        run(m, ms(1))
        assert m.periph.USART1.ISR_enabled.RXFNE == 1
        return device._traffic  # pyright: ignore[reportPrivateUsage]

    assert traffic_after_an_edge(Hearing())
    assert not traffic_after_an_edge(Stack())


STACK_VALUE: contextvars.ContextVar[str] = contextvars.ContextVar("stack_value", default="none")


def test_a_rerun_sets_the_context_every_later_hook_sees(u575: popcorn.PartDescription) -> None:
    seen: list[tuple[str, str]] = []
    builds = [0]

    def build(stack: Stack) -> None:
        builds[0] += 1
        STACK_VALUE.set(f"stack{builds[0]}")
        stack.loop.call_later(1e-5, lambda: seen.append(("timer", STACK_VALUE.get())))

    class Hearing(Stack):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            seen.append(("frame", STACK_VALUE.get()))

    m = asleep(u575, {"USART1": [Hearing(build)]})
    run(m, us(20))
    saved = m.snapshot()
    seen.clear()
    m.restore(saved)
    run(m, us(20))
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert seen == [("timer", "stack2"), ("frame", "stack2")]


KEPT_VALUE: contextvars.ContextVar[str] = contextvars.ContextVar("kept_value")


def test_a_snapshot_override_reads_the_stack_value_at_a_system_snapshot_and_restore(
    u575: popcorn.PartDescription,
) -> None:
    class Keeping(Stack):
        def on_attach(self) -> None:
            KEPT_VALUE.set("stack")

        def snapshot(self) -> bytes:
            return KEPT_VALUE.get().encode()

        def restore(self, data: bytes) -> None:
            KEPT_VALUE.set(data.decode())

    system, _, _ = member_pair(u575, 5, {"USART1": [Keeping()]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        system.restore(saved)
        system.snapshot()


def test_a_machine_reset_from_restore_nests_in_the_context(u575: popcorn.PartDescription) -> None:
    resets: list[popcorn.ResetKind] = []
    holder: list[popcorn.Machine] = []

    class Resetting(Saved):
        def reset(self, kind: popcorn.ResetKind) -> None:
            resets.append(kind)

        def restore(self, data: bytes) -> None:
            holder[0].reset(popcorn.ResetKind.SOFTWARE)

    m = asleep(u575, {"USART1": [Resetting()]})
    holder.append(m)
    run(m, us(5))
    saved = m.snapshot()
    resets.clear()
    m.restore(saved)
    assert resets == [popcorn.ResetKind.SOFTWARE]


def test_a_watch_from_the_test_thread_runs_in_the_machine_context(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[str] = []
    device = Stack(lambda stack: STACK_VALUE.set("stack"))
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    token = STACK_VALUE.set("test")
    device.board.watch_pin("PA0", lambda level, at: seen.append(STACK_VALUE.get()))
    STACK_VALUE.reset(token)
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    assert seen == ["stack"]


def test_a_value_of_the_first_run_ends_at_the_first_restore(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[str] = []

    def build(stack: Stack) -> None:
        seen.append(STACK_VALUE.get())
        STACK_VALUE.set("stack")

    token = STACK_VALUE.set("machine")
    m = asleep(u575, {"USART1": [Stack(build)]})
    STACK_VALUE.reset(token)
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    assert seen == ["machine", "machine"]


def test_a_restore_restarts_the_context_on_a_machine_with_no_loop(
    u575: popcorn.PartDescription,
) -> None:
    seen: list[str] = []

    class Reading(I2CTarget):
        def reset(self, kind: popcorn.ResetKind) -> None:
            seen.append(STACK_VALUE.get())
            STACK_VALUE.set("set")

    m = asleep(u575, {"I2C1": [Reading(0x38)]})
    saved = m.snapshot()
    seen.clear()
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert seen == ["set"]
    seen.clear()
    m.restore(saved)
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert seen == ["none"]


def test_a_value_set_in_restore_reaches_the_hooks(u575: popcorn.PartDescription) -> None:
    seen: list[str] = []

    class Setting(Saved):
        def restore(self, data: bytes) -> None:
            STACK_VALUE.set("restored")
            self.loop.call_later(1e-6, lambda: seen.append(STACK_VALUE.get()))

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            seen.append(STACK_VALUE.get())

    m = asleep(u575, {"USART1": [Setting()]})
    run(m, us(5))
    m.restore(m.snapshot())
    m.periph.USART1.TDR.value = 0x41
    run(m, ms(1))
    assert seen == ["restored", "restored"]


def test_a_close_from_a_finally_on_a_machine_with_no_device(u575: popcorn.PartDescription) -> None:
    m = asleep(u575)
    run(m, us(10))
    saved = m.snapshot()

    async def pending() -> None:
        try:
            await asyncio.sleep(3600)
        finally:
            m.loop.close()

    m.loop.create_task(pending())
    run(m, us(10))
    m.restore(saved)
    assert m.loop.is_closed()


def test_a_restore_then_a_close_runs_no_on_attach(u575: popcorn.PartDescription) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.restore(m.snapshot())
    m.loop.close()
    assert device.builds == 1


def test_a_rerun_left_held_after_a_close_keeps_nothing_alive(
    u575: popcorn.PartDescription,
) -> None:
    def body() -> weakref.ref[popcorn.Machine]:
        device = Stack(lambda stack: stack.board.watch_pin("PA0", lambda level, at: None))
        m = asleep(u575, {"USART1": [device]})
        run(m, us(5))
        m.restore(m.snapshot())
        device.loop.close()
        assert not device.loop._at_fire  # pyright: ignore[reportPrivateUsage]
        assert device.builds == 1
        return weakref.ref(m)

    assert collected(body())


def test_a_machine_dropped_with_a_rerun_pending_runs_no_on_attach(
    u575: popcorn.PartDescription,
) -> None:
    built: list[int] = []

    def body() -> weakref.ref[popcorn.Machine]:
        device = Stack(lambda stack: built.append(1))
        m = asleep(u575, {"USART1": [device]})
        run(m, us(5))
        m.restore(m.snapshot())
        return weakref.ref(m)

    machine = body()
    assert collected(machine)
    assert built == [1]


def test_a_rerun_asked_from_the_test_thread_runs_at_the_next_dispatch(
    u575: popcorn.PartDescription,
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    m.restore(m.snapshot())
    run(m, us(5))
    assert device.builds == 2
    UartDevice.restore(device, b"")
    run(m, us(5))
    assert device.builds == 3


def test_a_rerun_in_a_free_run_keeps_one_watch(u575: popcorn.PartDescription) -> None:
    def calls(free: bool) -> tuple[list[bool], int]:
        log: list[bool] = []
        device = Stack(
            lambda stack: stack.board.watch_pin("PA0", lambda level, at: log.append(level))
        )
        m = asleep(u575, {"USART1": [device]})
        run(m, us(5))
        saved_at = m.time
        m.restore(m.snapshot())

        async def goes_on() -> None:
            async with m.running():
                await m.sample(lambda: None, at=saved_at + us(2))
                assert device.builds == 2

        if free:
            asyncio.run(goes_on())
        run(m, us(5))
        m.periph.GPIOA.pin(0).level = True
        run(m, us(5))
        return log, device.builds

    assert calls(True) == calls(False) == ([True], 2)


def test_a_close_drops_what_a_reset_hook_left_ready(u575: popcorn.PartDescription) -> None:
    ran: list[str] = []

    class Readying(Stack):
        def reset(self, kind: popcorn.ResetKind) -> None:
            self.loop.call_soon(ran.append, "callback")

    m = asleep(u575, {"USART1": [Readying()]})
    ran.clear()
    m.reset(popcorn.ResetKind.SOFTWARE)
    m.loop.close()
    assert ran == []


def l152_device(device: UartDevice) -> popcorn.Machine:
    from popcorn.stm32 import mcu

    m = popcorn.Machine(mcu("STM32L152RETx"), devices={"USART1": [device]})
    m.reset()
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x400
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    return m


def test_an_l1_override_gets_its_bytes_back_and_a_default_device_reruns() -> None:
    override = Saved()
    m = l152_device(override)
    run(m, us(5))
    saved = m.snapshot()
    override.state = b"later"
    m.restore(saved)
    assert override.restored == [b"boot"]
    default = Stack()
    n = l152_device(default)
    run(n, us(5))
    n.restore(n.snapshot())
    run(n, us(5))
    assert default.builds == 2


def test_a_hook_error_at_a_system_restore_raises(u575: popcorn.PartDescription) -> None:
    class Failing(Saved):
        def restore(self, data: bytes) -> None:
            raise LookupError("from the hook")

    system, _, _ = member_pair(u575, 5, {"USART1": [Failing()]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        with pytest.raises(ModelError, match="from the hook"):
            system.restore(saved)


def test_a_rollback_runs_the_hook_once_with_the_state_kept(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = Saved()
    system, _, b = member_pair(u575, 5, {"USART1": [device]})
    with system:
        asyncio.run(system.run(for_=us(20)))
        saved = system.snapshot()
        device.state = b"later"
        device.restored.clear()
        real = popcorn.Machine._restore_state  # pyright: ignore[reportPrivateUsage]
        calls: list[int] = []

        def failing(self: popcorn.Machine, snapshot: popcorn.Snapshot) -> None:
            if self is b and not calls:
                calls.append(1)
                raise popcorn.ConfigError("refused", key="k", expected="e")
            real(self, snapshot)

        monkeypatch.setattr(popcorn.Machine, "_restore_state", failing)
        with pytest.raises(popcorn.ConfigError):
            system.restore(saved)
        assert device.restored == [b"later"]


def stopped_member(
    u575: popcorn.PartDescription, device: UartDevice | I2CTarget | SpiTarget, bus: str = "USART1"
) -> tuple[System, popcorn.SystemSnapshot]:
    system, _, b = member_pair(u575, 5, {bus: [device]})
    asyncio.run(system.run(for_=us(20)))
    saved = system.snapshot()
    b.mem.write(RAM, BKPT)
    b.reg[Reg.PC] = RAM
    stop = asyncio.run(system.run(for_=us(20)))
    assert stop.member == "b"
    assert system._undecided  # pyright: ignore[reportPrivateUsage]
    return system, saved


def test_a_refusing_override_leaves_a_member_stop_undelivered(
    u575: popcorn.PartDescription,
) -> None:
    class Refusing(Saved):
        refuses = False

        def snapshot(self) -> bytes:
            if self.refuses:
                raise SnapshotRefused("not now", device=repr(self))
            return super().snapshot()

    device = Refusing()
    system, saved = stopped_member(u575, device)
    with system:
        undecided = dict(system._undecided)  # pyright: ignore[reportPrivateUsage]
        device.refuses = True
        with pytest.raises(SnapshotRefused):
            system.restore(saved)
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]
        with pytest.raises(SnapshotRefused):
            system.snapshot()
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]


def test_an_override_returning_no_bytes_leaves_a_member_stop_undelivered(
    u575: popcorn.PartDescription,
) -> None:
    class Wrong(Saved):
        wrong = False

        def snapshot(self) -> bytes:
            return cast(bytes, 7) if self.wrong else super().snapshot()

    device = Wrong()
    system, saved = stopped_member(u575, device)
    with system:
        undecided = dict(system._undecided)  # pyright: ignore[reportPrivateUsage]
        device.wrong = True
        with pytest.raises(ModelError, match="returned a int"):
            system.restore(saved)
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]


class RefusingI2c(I2CTarget):
    refuses = False

    def snapshot(self) -> bytes:
        if self.refuses:
            raise SnapshotRefused("not now", device=repr(self))
        return super().snapshot()


class RefusingSpi(SpiTarget):
    refuses = False

    def snapshot(self) -> bytes:
        if self.refuses:
            raise SnapshotRefused("not now", device=repr(self))
        return super().snapshot()


@pytest.mark.parametrize("bus", ["I2C1", "SPI1"])
def test_a_refusing_target_leaves_a_member_stop_undelivered(
    u575: popcorn.PartDescription, bus: str
) -> None:
    device = RefusingI2c(0x38) if bus == "I2C1" else RefusingSpi()
    system, saved = stopped_member(u575, device, bus)
    with system:
        undecided = dict(system._undecided)  # pyright: ignore[reportPrivateUsage]
        device.refuses = True
        with pytest.raises(SnapshotRefused):
            system.restore(saved)
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]
        with pytest.raises(SnapshotRefused):
            system.snapshot()
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]


def test_a_refused_state_leaves_a_member_stop_undelivered(u575: popcorn.PartDescription) -> None:
    system, saved = stopped_member(u575, Saved())
    with system:
        undecided = dict(system._undecided)  # pyright: ignore[reportPrivateUsage]
        bad = type(saved)(dict(saved.members), b"not a state", saved.target)
        with pytest.raises(popcorn.ConfigError):
            system.restore(bad)
        assert system._undecided == undecided  # pyright: ignore[reportPrivateUsage]


def test_a_finally_error_becomes_a_note_on_a_failed_native_restore(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = Stack()
    m = asleep(u575, {"USART1": [device]})
    run(m, us(5))
    saved = m.snapshot()
    failing_finally(device.loop, "the finally")
    run(m, us(1))

    def refused(_: popcorn.Machine, __: popcorn.Snapshot) -> None:
        raise popcorn.ConfigError("refused", key="k", expected="e")

    monkeypatch.setattr(popcorn.Machine, "_restore_state", refused)
    with pytest.raises(popcorn.ConfigError) as raised:
        m.restore(saved)
    assert "the finally" in " ".join(raised.value.__notes__)


@pytest.mark.parametrize("tap", [False, True])
def test_a_tap_adds_no_traffic_and_taking_its_frames_clears_none(
    u575: popcorn.PartDescription, tap: bool
) -> None:
    device = Stack()
    device.tapping = tap
    m = asleep(u575, {"USART1": [device]})
    run(m, us(10))
    m.snapshot()
    device.loop.call_soon(sends, device)
    run(m, ms(1))
    assert len(device.tap_frames()) == (1 if tap else 0)
    with pytest.raises(SnapshotRefused):
        m.snapshot()


@pytest.mark.parametrize("tap", [False, True])
def test_a_tapped_boot_restored_goes_on_as_the_run_it_was_saved_from(
    u575: popcorn.PartDescription, tap: bool
) -> None:
    def later(stack: Stack) -> None:
        stack.loop.call_later(5e-4, stack.send, b"Z")

    device = Stack(later)
    device.tapping = tap
    m = asleep(u575, {"USART1": [device]})
    saved = m.snapshot()

    def outcome() -> tuple[object, ...]:
        run(m, ms(2))
        return (
            m.periph.USART1.RDR.value,
            m.time.ps,
            m.periph.USART1.ISR_enabled.RXFNE,
            list(device.log),
            device.tap_frames(),
        )

    went_on = outcome()
    m.restore(saved)
    device.log.clear()
    assert outcome() == went_on
    assert len(cast("list[object]", went_on[4])) == (1 if tap else 0)
