"""A 24xx EEPROM on I2C1 in fast mode: page write, acknowledge polling, random read, LL.

TIMINGR gives 400 kHz by RM0456's SCL formula without the tSYNC delays, which popcorn does not
model; the same TIMINGR runs slower on silicon.

The project file declares the EEPROM with a 5 ms write cycle. Two configs change the board:
`slow-eeprom` doubles the write cycle, `scripted` swaps the EEPROM for a `TableTarget` whose
reads come from a table. Without acknowledge polls, the scripted run takes the 9-bit frames of
its three transfers at that 400 kHz, plus start, stop and CPU time.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.devices import Eeprom24xx, TableTarget, TableWrite
from popcorn.pytest import MachineFactory
from popcorn.units import us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

PAGE = 0x0100
PAGE_BYTES = 32
SCRIPTED = bytes(0xC0 + index for index in range(PAGE_BYTES))
SCL_PERIOD = us(2.5)
BUS_BITS = 9 * ((1 + 2 + PAGE_BYTES) + (1 + 2) + (1 + PAGE_BYTES))


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="eeprom_done")
    assert stop.reason is StopReason.ADDRESS, stop


def page(machine: Machine, field: str) -> bytes:
    view = getattr(machine.var("eeprom_result"), field)
    return bytes(int(view[index].value) for index in range(PAGE_BYTES))


async def test_the_page_reads_back_and_lands_in_the_eeprom(machine: Machine) -> None:
    await run_to_done(machine)
    written = page(machine, "written")
    assert page(machine, "readback") == written

    (eeprom,) = machine.board.devices("I2C1")
    assert isinstance(eeprom, Eeprom24xx)
    assert eeprom.contents()[PAGE : PAGE + PAGE_BYTES] == written


async def test_acknowledge_polling_grows_with_the_write_cycle(
    machine: Machine, machine_factory: MachineFactory
) -> None:
    await run_to_done(machine)
    polls = machine.var("eeprom_result").nacked_polls.value
    assert isinstance(polls, int)
    assert polls > 0

    slow = await machine_factory.booted("slow-eeprom")
    await run_to_done(slow)
    slow_polls = slow.var("eeprom_result").nacked_polls.value
    assert isinstance(slow_polls, int)
    assert 1.9 * polls <= slow_polls <= 2.1 * polls


@pytest.mark.popcorn(config="scripted", budget=2_000_000)
async def test_a_scripted_target_answers_from_its_table(machine: Machine) -> None:
    start = machine.time
    await run_to_done(machine)
    elapsed = machine.time - start
    assert BUS_BITS * SCL_PERIOD.ps <= elapsed.ps < 1.25 * BUS_BITS * SCL_PERIOD.ps
    assert page(machine, "readback") == SCRIPTED
    assert machine.var("eeprom_result").nacked_polls.value == 0

    (target,) = machine.board.devices("I2C1")
    assert isinstance(target, TableTarget)
    assert target.writes() == (TableWrite(PAGE, page(machine, "written")),)
