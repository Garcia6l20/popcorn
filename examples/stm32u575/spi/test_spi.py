"""An AT25SF SPI NOR flash driven from a FreeRTOS task, the app behind a bootloader offset.

The `spi` fixture hands every test the firmware stopped at `main`, restored from one boot
snapshot, the flash erased. The module marker gives every run a budget in guest instructions,
so a firmware stuck in a loop fails the test with a BUDGET stop instead of hanging it.
"""

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.devices import At25sf
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

AT25SF641_JEDEC = [0x1F, 0x32, 0x17]
PAGE_ADDRESS = 0x1000
PAGE = bytes(0xA0 + index for index in range(16))
PAGE_PROGRAM_TIME = us(800)
VTOR = 0xE000_ED08


def flash(machine: Machine) -> At25sf:
    (device,) = machine.board.devices("SPI1")
    assert isinstance(device, At25sf)
    return device


def stop_on_failure(machine: Machine) -> None:
    for handler in ("vAssertCalled", "Error_Handler"):
        machine.intercept(handler, lambda call: call.stop())


async def run_flash_task(machine: Machine) -> None:
    stop_on_failure(machine)
    stop = await machine.run(until="flash_task_done")
    assert stop.reason is StopReason.ADDRESS, stop


async def test_the_firmware_reads_the_jedec_id(spi: Machine) -> None:
    await run_flash_task(spi)
    jedec = spi.var("flash_result").jedec
    assert [jedec[index].value for index in range(3)] == AT25SF641_JEDEC


async def test_a_page_program_lands_in_the_flash_after_tpp(spi: Machine) -> None:
    times: dict[str, VirtualTime] = {}
    for hook in ("flash_page_program_sent", "flash_page_program_done"):

        def timestamp(call: Call, hook: str = hook) -> Return:
            times[hook] = spi.time
            return call.proceed()

        spi.intercept(hook, timestamp)
    await run_flash_task(spi)

    result = spi.var("flash_result")
    assert bytes(int(result.readback[index].value) for index in range(16)) == PAGE
    assert flash(spi).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == PAGE
    assert result.status_polls.value > 1
    busy = times["flash_page_program_done"] - times["flash_page_program_sent"]
    assert PAGE_PROGRAM_TIME <= busy < PAGE_PROGRAM_TIME + ms(1)


def test_every_test_starts_from_the_boot_snapshot_with_the_flash_erased(spi: Machine) -> None:
    assert flash(spi).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == b"\xff" * 16
    assert spi.var("flash_result").done.value == 0
    assert spi.pc == spi.sym("main")


async def test_a_snapshot_restores_the_flash_contents_with_the_machine(spi: Machine) -> None:
    before = spi.snapshot()
    await run_flash_task(spi)
    assert flash(spi).contents()[PAGE_ADDRESS] == PAGE[0]

    spi.restore(before)
    assert flash(spi).contents()[PAGE_ADDRESS] == 0xFF
    await run_flash_task(spi)
    assert flash(spi).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == PAGE


async def test_the_app_runs_both_tasks_from_its_relocated_vector_table(spi: Machine) -> None:
    await run_flash_task(spi)
    stop = await spi.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    vtor = spi.mem.u32[VTOR]
    assert vtor == spi.sym("ram_vectors")
    assert vtor & 0x7F == 0
    assert spi.var("ticker_runs").value >= 5
