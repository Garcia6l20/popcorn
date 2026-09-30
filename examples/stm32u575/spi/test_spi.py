"""An AT25SF SPI NOR flash driven from a FreeRTOS task, the app behind a bootloader offset.

The `machine` fixture hands every test the firmware stopped at `main`, restored from one boot
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


async def test_the_firmware_reads_the_jedec_id(machine: Machine) -> None:
    await run_flash_task(machine)
    jedec = machine.var("flash_result").jedec
    assert [jedec[index].value for index in range(3)] == AT25SF641_JEDEC


async def test_a_page_program_lands_in_the_flash_after_tpp(machine: Machine) -> None:
    times: dict[str, VirtualTime] = {}
    for hook in ("flash_page_program_sent", "flash_page_program_done"):

        def timestamp(call: Call, hook: str = hook) -> Return:
            times[hook] = machine.time
            return call.proceed()

        machine.intercept(hook, timestamp)
    await run_flash_task(machine)

    result = machine.var("flash_result")
    assert bytes(int(result.readback[index].value) for index in range(16)) == PAGE
    assert flash(machine).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == PAGE
    assert result.status_polls.value > 1
    busy = times["flash_page_program_done"] - times["flash_page_program_sent"]
    assert PAGE_PROGRAM_TIME <= busy < PAGE_PROGRAM_TIME + ms(1)


def test_every_test_starts_from_the_boot_snapshot_with_the_flash_erased(machine: Machine) -> None:
    assert flash(machine).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == b"\xff" * 16
    assert machine.var("flash_result").done.value == 0
    assert machine.pc == machine.sym("main")


async def test_a_snapshot_restores_the_flash_contents_with_the_machine(machine: Machine) -> None:
    before = machine.snapshot()
    await run_flash_task(machine)
    assert flash(machine).contents()[PAGE_ADDRESS] == PAGE[0]

    machine.restore(before)
    assert flash(machine).contents()[PAGE_ADDRESS] == 0xFF
    await run_flash_task(machine)
    assert flash(machine).contents()[PAGE_ADDRESS : PAGE_ADDRESS + len(PAGE)] == PAGE


async def test_the_app_runs_both_tasks_from_its_relocated_vector_table(machine: Machine) -> None:
    await run_flash_task(machine)
    stop = await machine.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    vtor = machine.mem.u32[VTOR]
    assert vtor == machine.sym("ram_vectors")
    assert vtor & 0x7F == 0
    assert machine.var("ticker_runs").value >= 5
