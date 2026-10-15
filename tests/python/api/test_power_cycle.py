"""`Machine.power_cycle()`: RAM lost, flash and bus device arrays kept, the machine reset."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn import ResetKind, StopReason, System, UartLink
from popcorn.devices import At25sf
from popcorn.units import VirtualTime, ms, us

from .conftest import BOARD_ELF
from .linked import member
from .test_devices import BUDGET, SPI_ELF, enter, finish

pytest_plugins = ["pytester"]

SRAM = 0x2000_0000
SRAM_SIZE = 0xC_0000
SRAM_SECURE = 0x3000_0000
BKPSRAM = 0x4003_6400
TAG = SRAM + 0x4_0000
FLASH_WORD = 0x080F_0000
GPIOA_BSRR = 0x4202_0018
CS_PIN = 1 << 4
ERASE_4K = ms(2)


def to_ready(m: popcorn.Machine) -> None:
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    assert stop.reason is StopReason.ADDRESS


def power_on_reset(m: popcorn.Machine) -> None:
    m.reset(ResetKind.POWER_ON)


def power_cycle(m: popcorn.Machine) -> None:
    m.power_cycle()


def test_ram_reads_0_flash_keeps_its_bytes_and_the_clock_restarts(board: popcorn.Machine) -> None:
    board.mem.u32[TAG] = 0xA5A5_5A5A
    board.mem.u32[BKPSRAM] = 0x1234_5678
    board.mem.u32[FLASH_WORD] = 0xC0DE_F00D
    asyncio.run(board.run(for_=us(20)))
    assert board.mem.u32[SRAM_SECURE + 0x4_0000] == 0xA5A5_5A5A
    board.power_cycle()
    assert board.time == VirtualTime(0)
    assert board.mem.u32[TAG] == 0
    assert board.mem.u32[SRAM_SECURE + 0x4_0000] == 0
    assert board.mem.u32[BKPSRAM] == 0
    assert board.mem.read(SRAM, SRAM_SIZE) == bytes(SRAM_SIZE)
    assert board.mem.u32[FLASH_WORD] == 0xC0DE_F00D
    assert board.last_stop is None
    to_ready(board)
    assert board.mem.u32[FLASH_WORD] == 0xC0DE_F00D


@pytest.mark.parametrize("cycle", [power_cycle, power_on_reset], ids=["power_cycle", "reset"])
def test_a_pending_edge_keeps_its_instant_after_a_system_reset_then_the_cycle(
    board: popcorn.Machine, cycle: Callable[[popcorn.Machine], None]
) -> None:
    asyncio.run(board.run(for_=us(20)))
    board.reset(ResetKind.SOFTWARE)
    to_ready(board)
    pin = board.periph.GPIOA.pin(0)
    due = board.time + us(5000)
    asyncio.run(pin.drive_at(True, at=due))
    asyncio.run(board.run(for_=us(1000)))
    cut_at = board.time
    cycle(board)
    to_ready(board)
    lands = VirtualTime(due.ps - cut_at.ps)
    assert board.time < lands
    asyncio.run(board.run(for_=lands - board.time - us(50)))
    assert not pin.level
    asyncio.run(board.run(for_=us(100)))
    assert pin.level


def test_an_at25sf_keeps_its_array_and_loses_wel(u575: popcorn.PartDescription) -> None:
    flash = At25sf("PA4")
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [flash]})
    enter(m, "write_read")
    assert finish(m)[1] == 16
    written = flash.contents()
    assert written[0x1000:0x1010] == bytes(range(0xA0, 0xB0))
    enter(m, "write_read")
    fresh = flash.snapshot()
    command = m.sym("flash_command")
    for _ in range(2):
        assert asyncio.run(m.run(until=command, budget=1_000_000)).reason is StopReason.ADDRESS
    enabled = flash.snapshot()
    assert enabled != fresh
    m.power_cycle()
    assert flash.snapshot() == fresh
    assert flash.contents() == written


def erase_cut_halfway(
    u575: popcorn.PartDescription, running: bool, software_reset: bool
) -> tuple[VirtualTime, VirtualTime, bytes]:
    """Run the SPI firmware's 4 KB erase of 0x1000 over zeros to the first status poll past half
    its time, parked or in a free run, then power cycle. After a software reset, `m.time` runs
    behind the time since power-on. Returns the CS rise that launched the erase, the cut time and
    the sector after the cycle."""
    flash = At25sf("PA4", initial=bytes(0x2000), erase_4k=ERASE_4K)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [flash]})
    if software_reset:
        m.reset()
        asyncio.run(m.run(for_=us(50)))
    enter(m, "erase", ResetKind.SOFTWARE if software_reset else ResetKind.POWER_ON)
    raised: list[VirtualTime] = []
    launched: list[VirtualTime] = []

    def deselected(hit: popcorn.WatchHit) -> None:
        if hit.value & CS_PIN:
            raised.append(m.time)

    def halfway(call: popcorn.Call) -> popcorn.Return:
        if call.occurrence == 2:
            launched.append(raised[-1])
        if launched and m.time >= launched[0] + ERASE_4K // 2:
            return call.stop()
        return call.proceed()

    async def free() -> popcorn.Stop:
        with pytest.raises(popcorn.EmulationError, match="stopped with USER") as ended:
            async with m.running():
                await m.wait_for(popcorn.Reached("main"), for_=ERASE_4K)
        assert ended.value.stop is not None
        return ended.value.stop

    with m.watch(GPIOA_BSRR, handler=deselected), m.intercepted("flash_command", halfway):
        stop = asyncio.run(free()) if running else asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER
    assert m.pc == m.sym("flash_command")
    cut_at = m.time
    assert cut_at == stop.time
    m.power_cycle()
    return launched[0], cut_at, flash.contents()[0x1000:0x2000]


@pytest.mark.parametrize("software_reset", [False, True], ids=["power_on", "software_reset"])
def test_an_at25sf_erase_cut_halfway_keeps_a_prefix_parked_and_running(
    u575: popcorn.PartDescription, software_reset: bool
) -> None:
    launch, cut_at, sector = erase_cut_halfway(u575, False, software_reset)
    kept = 0x1000 * (cut_at - launch).ps // ERASE_4K.ps
    assert 0x800 <= kept < 0x880
    assert sector == b"\xff" * kept + bytes(0x1000 - kept)
    assert erase_cut_halfway(u575, True, software_reset) == (launch, cut_at, sector)


def test_probes_and_intercepts_stay_installed(board: popcorn.Machine) -> None:
    calls: list[int] = []

    def seen(call: popcorn.Call) -> popcorn.Return:
        calls.append(call.occurrence)
        return call.proceed()

    with board.probe("ready") as probe, board.intercepted("ready", seen):
        board.power_cycle()
        asyncio.run(board.run(for_=us(20)))
        assert probe.count == 1
        assert calls == [0]


def test_a_running_machine_refuses_a_power_cycle_as_it_refuses_a_reset(
    board: popcorn.Machine,
) -> None:
    board.mem.u32[TAG] = 0xA5A5_5A5A
    board.start()
    for call in (board.power_cycle, board.reset):
        with pytest.raises(popcorn.MachineRunning, match="the machine is running"):
            call()
    board.halt()
    assert board.mem.u32[TAG] == 0xA5A5_5A5A
    assert asyncio.run(board.run(for_=us(20))).reason is StopReason.TIME


def test_a_handler_gets_the_same_refusal_from_a_power_cycle_and_a_reset(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    m.mem.u32[TAG] = 0xA5A5_5A5A
    refused: list[tuple[type[BaseException], str]] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        for cycle in (call.machine.power_cycle, call.machine.reset):
            try:
                cycle()
            except popcorn.PopcornError as error:
                refused.append((type(error), str(error)))
        return call.stop()

    with m.intercepted("ready", handler):
        stop = asyncio.run(m.run(budget=1_000_000))
    assert stop.reason is StopReason.USER
    assert [kind for kind, _ in refused] == [popcorn.MachineRunning, popcorn.MachineRunning]
    assert all("running a slice" in text for _, text in refused)
    assert m.mem.u32[TAG] == 0xA5A5_5A5A
    assert m.pc == m.sym("ready")


def test_a_linked_member_refuses_a_power_cycle(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    link = UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)
    with (
        System({"a": a, "b": b}, [link]),
        pytest.raises(popcorn.MachineBusy, match="a is a member of a linked system"),
    ):
        a.power_cycle()


@pytest.fixture(scope="session")
def u575_json(u575: popcorn.PartDescription) -> Path:
    """The cached STM32U575 description file, found before pytester moves HOME."""
    del u575
    from popcorn.stm32.description import cached_description

    return cached_description("STM32U575QGIx")


def test_the_next_test_of_a_fixture_gets_the_boot_snapshot_back(
    u575_json: Path, pytester: pytest.Pytester
) -> None:
    pytester.makeconftest(
        "from pathlib import Path\n\nimport popcorn\nimport popcorn.pytest\n\n"
        'pytest_plugins = ["popcorn.pytest"]\n\n\n'
        "@popcorn.pytest.machine\n"
        "def board():\n"
        "    return popcorn.MachineDescription(\n"
        '        part="STM32U575QGIx",\n'
        f"        description=Path({str(u575_json)!r}),\n"
        f"        images=[popcorn.Elf(Path({str(BOARD_ELF)!r}))],\n"
        "        boot=popcorn.Boot(\n"
        '            until="ready",\n'
        "            budget=1_000_000,\n"
        '            memory_init=[popcorn.MemoryInit("_ebss", bytes(range(1, 17)))],\n'
        "        ),\n"
        "    )\n"
    )
    pytester.makepyfile(
        "import hashlib\nfrom pathlib import Path\n\n"
        f"SRAM, SIZE = {SRAM:#x}, {SRAM_SIZE:#x}\n\n\n"
        "def test_cut(board):\n"
        "    assert board.mem.read(board.sym('_ebss'), 16) == bytes(range(1, 17))\n"
        "    sram = board.mem.read(SRAM, SIZE)\n"
        "    Path('boot.txt').write_text(hashlib.sha256(sram).hexdigest())\n"
        "    board.power_cycle()\n"
        "    assert board.mem.read(SRAM, SIZE) == bytes(SIZE)\n\n\n"
        "def test_next(board):\n"
        "    sram = board.mem.read(SRAM, SIZE)\n"
        "    assert hashlib.sha256(sram).hexdigest() == Path('boot.txt').read_text()\n"
        "    assert board.mem.read(board.sym('_ebss'), 16) == bytes(range(1, 17))\n"
    )
    result = pytester.runpytest("-p", "no:asyncio", "-p", "no:randomly")
    result.assert_outcomes(passed=2)
