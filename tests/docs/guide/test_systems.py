import asyncio
from collections.abc import Sequence
from pathlib import Path
from typing import cast

import popcorn
import popcorn.pytest
import pytest
from popcorn import LineState, StopReason, System
from popcorn.board import I2CController
from popcorn.stm32 import mcu
from popcorn.units import VirtualTime, ms, us

PREBUILT = Path(__file__).parents[3] / "examples/stm32u575/linked_uart/prebuilt"
LINK_FIRMWARE = Path(__file__).parents[2] / "firmware/periph/prebuilt"
SCRIPT_AREA = 0x20080000
MASTER_ROLE = 1
SLAVE_ROLE = 2


def link_member(
    elf: str, name: str, role: int, script: Sequence[int] = (), own: int = 0, spi: bool = False
) -> popcorn.Machine:
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"), image=popcorn.Image.elf(LINK_FIRMWARE / elf), name=name
    )
    machine.reset()
    asyncio.run(machine.run(until="main", budget=1_000_000))
    words = machine.mem.u32
    if spi:
        head, first = (0x5B1A11, role, 0, 0, 0, 0), SCRIPT_AREA + 24
    else:
        head, first = (0x12C11B, role, 0, 0, 0, 0, own), SCRIPT_AREA + 40
    for index, value in enumerate(head):
        words[SCRIPT_AREA + 4 * index] = value
    for index in range(64):
        words[first + 4 * index] = script[index] if index < len(script) else 0
    machine.reg.pc = machine.sym("link_run")
    return machine


def spi_script() -> list[int]:
    cs_high, wait_us, cs_low, send = 2 << 24 | 4, 4 << 24 | 50, 1 << 24 | 4, 3 << 24 | 3 << 8 | 0x10
    return [cs_high, wait_us, cs_low, send, cs_high]


def i2c_script() -> list[int]:
    wait_us, write, read = (
        4 << 24 | 50,
        1 << 24 | 0x42 << 16 | 3 << 8 | 0x10,
        2 << 24 | 0x42 << 16 | 2 << 8,
    )
    return [wait_us, write, read]


# --8<-- [start:describe]
CONTROLLER = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "linked_controller.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)
RESPONDER = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "linked_responder.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)
SYSTEM = popcorn.SystemDescription(
    members={"controller": CONTROLLER, "responder1": RESPONDER, "responder2": RESPONDER},
    links=[
        popcorn.UartLink("controller.UART4", "responder1.USART3", latency=us(20), baud=460_800),
        popcorn.UartLink("controller.USART3", "responder2.USART3", latency=us(20), baud=460_800),
    ],
)


@popcorn.pytest.system
def system() -> popcorn.SystemDescription:
    return SYSTEM


# --8<-- [end:describe]


async def test_the_members_run_on_one_clock(system: System) -> None:
    # --8<-- [start:run]
    stop = await system.run(for_=ms(1))
    assert stop.reason is StopReason.TIME
    assert system.members["responder1"].var("node_id").value == 1
    assert system.members["responder2"].var("node_id").value == 2
    # --8<-- [end:run]


async def test_a_tap_shows_every_frame_with_its_times(system: System) -> None:
    # --8<-- [start:tap]
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    frames = tap.frames(towards="responder1.USART3")
    assert bytes(frame.data for frame in frames[:2]) == b"N\x01"
    assert all(frame.arrival == frame.end + us(20) for frame in frames)
    # --8<-- [end:tap]


async def test_a_cut_loses_the_frames_inside_it(system: System) -> None:
    # --8<-- [start:cut]
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    tap.frames()
    now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
    start = VirtualTime(now.ps + ms(15).ps)
    tap.cut(start, VirtualTime(start.ps + ms(30).ps), line="break")
    await system.run(for_=ms(60))
    cut = [frame for frame in tap.frames() if frame.cut]
    assert cut
    assert all(frame.start.ps < start.ps + ms(30).ps for frame in cut)
    # --8<-- [end:cut]


async def test_a_snapshot_restores_every_member(system: System) -> None:
    # --8<-- [start:snapshot]
    await system.run(for_=ms(1))
    snapshot = system.snapshot()
    before = system.members["controller"].time
    await system.run(for_=ms(20))
    system.restore(snapshot)
    assert system.members["controller"].time == before
    # --8<-- [end:snapshot]


async def test_a_member_waits_while_the_system_runs(system: System) -> None:
    # --8<-- [start:running]
    controller = system.members["controller"]
    async with system.running():
        await controller.elapse(ms(1))
    assert system.members["responder1"].var("node_id").value == 1
    # --8<-- [end:running]


@pytest.mark.parametrize(("line", "errors"), [("idle", 0), ("break", 1)])
async def test_a_break_line_ends_the_reception_with_an_error(
    system: System, line: str, errors: int
) -> None:
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    failed = system.members["responder1"].probe("HAL_UART_ErrorCallback")
    now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
    start = VirtualTime(now.ps + ms(15).ps)
    tap.cut(start, VirtualTime(start.ps + ms(30).ps), line=cast(LineState, line))
    await system.run(for_=ms(60))
    assert (failed.count > 0) == bool(errors)


async def test_a_script_boots_and_links_in_one_call() -> None:
    system = await SYSTEM.boot()
    try:
        stop = await system.run(for_=ms(1))
        assert stop.reason is StopReason.TIME
        assert system.members["responder1"].var("node_id").value == 1
    finally:
        system.close()


@pytest.fixture
def spi_members() -> tuple[popcorn.Machine, popcorn.Machine]:
    return (
        link_member("spi_ll.elf", "a", MASTER_ROLE, spi_script(), spi=True),
        link_member("spi_ll.elf", "b", SLAVE_ROLE, spi=True),
    )


@pytest.fixture
def i2c_members() -> tuple[popcorn.Machine, popcorn.Machine]:
    return (
        link_member("i2c_ll.elf", "a", MASTER_ROLE, i2c_script()),
        link_member("i2c_ll.elf", "b", SLAVE_ROLE, own=0x42),
    )


async def test_an_spi_bus_gives_the_master_the_slave_s_replies(
    spi_members: tuple[popcorn.Machine, popcorn.Machine],
) -> None:
    master, slave = spi_members
    # --8<-- [start:spi]
    bus = popcorn.SpiBus(master="a.SPI1", slaves={"a.PA4": "b.SPI1"}, latency=us(20))
    system = System({"a": master, "b": slave}, [bus])
    try:
        await system.run(for_=ms(1))
        tap = system.tap("a.SPI1", popcorn.SpiTap)
        frames = tap.frames()
        assert [(frame.mosi, frame.miso) for frame in frames] == [
            (0x10, 0xA0),
            (0x11, 0x20),
            (0x12, 0x21),
        ]
        assert all(frame.reply.ps == frame.at.ps + us(20).ps + us(4).ps for frame in frames)
        spacing = frames[2].at.ps - frames[1].at.ps
        assert spacing >= 2 * us(20).ps + 2 * us(4).ps
    finally:
        system.close()
    # --8<-- [end:spi]
    assert spacing == 2 * us(20).ps + 2 * us(4).ps + us(6.25).ps


async def test_an_i2c_bus_gives_the_controller_the_target_s_answers(
    i2c_members: tuple[popcorn.Machine, popcorn.Machine],
) -> None:
    controller, target = i2c_members
    # --8<-- [start:i2c]
    bus = popcorn.I2CBus(ports=["a.I2C1", "b.I2C1"], latency=us(20))
    system = System({"a": controller, "b": target}, [bus])
    try:
        await system.run(for_=ms(1))
        tap = system.tap("a.I2C1", popcorn.I2CTap)
        phases = tap.phases(target="b.I2C1")
        assert [(phase.phase, phase.byte, phase.ack) for phase in phases[:3]] == [
            (popcorn.board.I2CPhase.ADDRESS, 0x42 << 1, True),
            (popcorn.board.I2CPhase.WRITE, 0x10, True),
            (popcorn.board.I2CPhase.WRITE, 0x11, True),
        ]
        scl_period = us(2.5)
        byte_time = 9 * scl_period.ps
        write = phases[1]
        assert write.answer.ps == write.at.ps + us(20).ps + byte_time
        assert phases[2].at.ps - phases[1].at.ps == 2 * us(20).ps + 2 * byte_time
    finally:
        system.close()
    # --8<-- [end:i2c]


async def test_an_i2c_controller_addresses_a_target_mode_firmware() -> None:
    # --8<-- [start:controller]
    host = I2CController(scl_hz=100_000)
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(LINK_FIRMWARE / "i2c_ll.elf"),
        devices={"I2C1": [host]},
    )
    machine.reset()
    machine.reg.pc = machine.sym("target")
    await machine.run(for_=us(200))
    host.start(0x42, at=VirtualTime(machine.time.ps + us(10).ps))
    host.write(b"\x11")
    host.stop(at=VirtualTime(machine.time.ps + us(500).ps))
    await machine.run(for_=ms(1))
    assert [(answer.phase, answer.ack) for answer in host.answers] == [
        (popcorn.board.I2CPhase.ADDRESS, True),
        (popcorn.board.I2CPhase.WRITE, True),
    ]
    # --8<-- [end:controller]
