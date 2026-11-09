"""The clock the MCU drives onto a pin, read and watched from a bus device: `pin_clock` and
`watch_pin_clock` on the LSCO pin of the STM32U5, STM32H5, STM32L4 and STM32L1."""

from __future__ import annotations

import asyncio
from typing import override

import popcorn
import pytest
from popcorn import ConfigError, SnapshotRefused
from popcorn.arm import Reg
from popcorn.board import RccOptions, UartDevice
from popcorn.stm32 import mcu
from popcorn.units import VirtualTime, ms, us

BRANCH_TO_SELF = b"\xfe\xe7"
RAM = 0x2000_0000
THUMB = 1 << 24
RCC_BDCR = 0x4602_0CF0
LSION = 1 << 26
LSCOEN = 1 << 24


class Plain(UartDevice):
    pass


def started(
    description: popcorn.PartDescription, device: UartDevice, code: bytes = BRANCH_TO_SELF
) -> popcorn.Machine:
    m = popcorn.Machine(description, devices={"USART1": [device]}, rcc=RccOptions(lsi_hz=31_550))
    m.reset()
    m.mem.write(RAM, code)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    return m


def run(m: popcorn.Machine, microseconds: int) -> None:
    asyncio.run(m.run(for_=us(microseconds)))


def test_the_lsco_pin_follows_a_parked_write_from_lsi_to_lse_to_off_at_exact_instants(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = started(u575, device)
    calls: list[tuple[int, VirtualTime]] = []
    watch = device.board.watch_pin_clock("PA2", lambda hz, at: calls.append((hz, at)))
    assert device.board.pin_clock("PA2") == 0
    bdcr = m.periph.RCC.RCC_BDCR
    first = m.time
    bdcr.LSION = 1
    bdcr.LSCOEN = 1
    run(m, 300)
    assert calls == [(31_550, VirtualTime(first.ps + 100_000_000))]
    assert device.board.pin_clock("PA2") == 31_550
    bdcr.LSEON = 1
    run(m, 300)
    assert len(calls) == 1
    second = m.time
    bdcr.LSCOSEL = 1
    run(m, 10)
    assert calls[-1] == (0, second)
    assert device.board.pin_clock("PA2") == 0
    bdcr.LSESYSEN = 1
    run(m, 300)
    assert calls[-1][0] == 32_768
    assert device.board.pin_clock("PA2") == 32_768
    third = m.time
    bdcr.LSCOEN = 0
    run(m, 10)
    assert calls[-1] == (0, third)
    assert [hz for hz, _ in calls] == [31_550, 0, 32_768, 0]
    stamps = [at.ps for _, at in calls]
    assert stamps == sorted(stamps)
    assert device.board.pin_clock("PA3") == 0
    watch.remove()
    bdcr.LSCOEN = 1
    run(m, 10)
    assert len(calls) == 4


def test_a_pin_clock_refuses_what_pin_level_refuses(u575: popcorn.PartDescription) -> None:
    device = Plain()
    started(u575, device)
    board = device.board
    with pytest.raises(TypeError):
        board.pin_clock(5)  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        board.pin_clock("PA16")
    with pytest.raises(TypeError):
        board.watch_pin_clock("PA2", 5)  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        board.watch_pin_clock("PA16", lambda hz, at: None)
    with pytest.raises(ConfigError):
        Plain().board.pin_clock("PA2")
    with pytest.raises(ConfigError):
        Plain().board.watch_pin_clock("PA2", lambda hz, at: None)


class Stack(UartDevice):
    """Watches PA2 from `on_attach`, so the watch belongs to its stack."""

    def __init__(self) -> None:
        super().__init__()
        self.builds = 0
        self.calls: list[int] = []

    @override
    def on_attach(self) -> None:
        self.builds += 1
        self.board.watch_pin_clock("PA2", lambda hz, at: self.calls.append(hz))


def lsco_on(m: popcorn.Machine) -> None:
    m.periph.RCC.RCC_BDCR.LSION = 1
    m.periph.RCC.RCC_BDCR.LSCOEN = 1


def test_a_rate_delivered_to_a_watch_of_the_stack_refuses_the_default_snapshot(
    u575: popcorn.PartDescription,
) -> None:
    stack = Stack()
    m = started(u575, stack)
    m.snapshot()
    lsco_on(m)
    run(m, 300)
    assert stack.calls == [31_550]
    with pytest.raises(SnapshotRefused):
        m.snapshot()
    control = Plain()
    other = started(u575, control)
    heard: list[int] = []
    control.board.watch_pin_clock("PA2", lambda hz, at: heard.append(hz))
    lsco_on(other)
    run(other, 300)
    assert heard == [31_550]
    other.snapshot()


def test_a_rerun_of_on_attach_leaves_one_clock_watch_one_call_per_change(
    u575: popcorn.PartDescription,
) -> None:
    stack = Stack()
    m = started(u575, stack)
    run(m, 1)
    saved = m.snapshot()
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    size = len(table)
    for _ in range(3):
        m.restore(saved)
        run(m, 1)
    assert stack.builds == 4
    assert len(table) == size
    lsco_on(m)
    run(m, 300)
    assert stack.calls == [31_550]


def lsco_store() -> bytes:
    return (
        bytes.fromhex("0148" "0249" "0160" "fee7")
        + RCC_BDCR.to_bytes(4, "little")
        + (LSION | LSCOEN).to_bytes(4, "little")
    )


def test_a_firmware_store_reaches_the_watch_the_same_parked_and_in_a_free_run(
    u575: popcorn.PartDescription,
) -> None:
    parked_device = Plain()
    parked_machine = started(u575, parked_device, lsco_store())
    parked_calls: list[tuple[int, int]] = []
    parked_device.board.watch_pin_clock("PA2", lambda hz, at: parked_calls.append((hz, at.ps)))
    run(parked_machine, 300)
    assert [hz for hz, _ in parked_calls] == [31_550]
    assert 100_000_000 <= parked_calls[0][1] < 100_000_000 + 2_000_000

    running_device = Plain()
    running_machine = started(u575, running_device, lsco_store())
    running_calls: list[tuple[int, int]] = []
    stamps: list[int] = []

    def heard(hz: int, at: VirtualTime) -> None:
        running_calls.append((hz, at.ps))
        stamps.append(round(running_device.loop.time() * 10**12))

    running_device.board.watch_pin_clock("PA2", heard)

    async def body() -> None:
        async with running_machine.running():
            held = await running_machine.wait_until(lambda _: bool(running_calls), for_=ms(5))
            assert held is not None

    asyncio.run(body())
    assert running_calls == parked_calls
    assert stamps == [at for _, at in running_calls]


def test_a_watch_stays_across_a_reset_which_calls_with_the_new_rate(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = started(u575, device)
    calls: list[tuple[int, int]] = []
    device.board.watch_pin_clock("PA2", lambda hz, at: calls.append((hz, at.ps)))
    m.periph.RCC.RCC_BDCR.LSION = 1
    run(m, 300)
    m.periph.RCC.RCC_BDCR.LSCOEN = 1
    m.reset()
    run(m, 10)
    assert device.board.pin_clock("PA2") == 0
    assert calls == [(0, 0)]
    calls.clear()
    m.reset()
    run(m, 10)
    assert calls == []
    m.mem.write(RAM, BRANCH_TO_SELF)
    m.reg[Reg.PC] = RAM
    lsco_on(m)
    run(m, 300)
    assert [hz for hz, _ in calls] == [31_550]


def test_a_restore_brings_back_the_pending_call_and_keeps_the_watch(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = started(u575, device)
    calls: list[int] = []
    device.board.watch_pin_clock("PA2", lambda hz, at: calls.append(hz))
    m.periph.RCC.RCC_BDCR.LSION = 1
    run(m, 300)
    m.periph.RCC.RCC_BDCR.LSCOEN = 1
    saved = m.snapshot()
    run(m, 10)
    assert calls == [31_550]
    m.periph.RCC.RCC_BDCR.LSCOEN = 0
    run(m, 10)
    assert calls == [31_550, 0]
    calls.clear()
    m.restore(saved)
    assert device.board.pin_clock("PA2") == 31_550
    run(m, 10)
    assert calls == [31_550]
    m.periph.RCC.RCC_BDCR.LSCOEN = 0
    run(m, 10)
    assert calls == [31_550, 0]


def test_a_restore_from_a_quiet_snapshot_calls_no_watcher(u575: popcorn.PartDescription) -> None:
    device = Plain()
    m = started(u575, device)
    calls: list[int] = []
    device.board.watch_pin_clock("PA2", lambda hz, at: calls.append(hz))
    saved = m.snapshot()
    lsco_on(m)
    run(m, 300)
    assert calls == [31_550]
    calls.clear()
    m.restore(saved)
    assert device.board.pin_clock("PA2") == 0
    assert calls == []
    run(m, 300)
    assert calls == []


@pytest.mark.parametrize(
    ("part", "register", "pin", "other"),
    [
        ("STM32H573AIIx", "RCC_BDCR", "PB2", "PA2"),
        ("STM32L476RGTx", "BDCR", "PA2", "PB2"),
    ],
)
def test_the_lsco_pin_of_the_h5_and_l4(part: str, register: str, pin: str, other: str) -> None:
    from popcorn.stm32.description import DescriptionError

    try:
        description = mcu(part)
    except DescriptionError as error:
        pytest.skip(str(error))
    device = Plain()
    m = started(description, device)
    rcc = m.periph.RCC
    if part.startswith("STM32L4"):
        rcc.CSR.LSION = 1
    else:
        rcc.RCC_BDCR.LSION = 1
    bdcr = getattr(rcc, register)
    bdcr.LSCOEN = 1
    run(m, 300)
    assert device.board.pin_clock(pin) == 31_550
    assert device.board.pin_clock(other) == 0


def test_the_stm32l1_has_no_clock_output() -> None:
    from popcorn.stm32.description import DescriptionError

    try:
        description = mcu("STM32L152RETx")
    except DescriptionError as error:
        pytest.skip(str(error))
    device = Plain()
    m = started(description, device)
    m.periph.RCC.CSR.LSION = 1
    run(m, 300)
    assert device.board.pin_clock("PA2") == 0
    assert device.board.pin_clock("PB2") == 0
