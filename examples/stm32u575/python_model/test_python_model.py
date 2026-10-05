"""The RNG, which popcorn does not model, served by a model written in Python.

The firmware asks the HAL for eight random words, each by `HAL_RNG_GenerateRandomNumber_IT`,
and stores them from `HAL_RNG_ReadyDataCallback`. `rngmodel.Rng` is a `popcorn.model.Model`:
the project puts it in the RNG slot with `Periph(model=Use(Rng))`, and its words come from a
seeded sequence the test computes too.

`HAL_RNG_Init` waits for the first DRDY without reading DR, so the first callback comes as
soon as the interrupt is enabled. Each later word waits one conversion of the model. The
interval check allows up to twice that: the IRQ entry and the HAL handler add a few
microseconds at 4 MHz.

The first test builds its own machine without the model, from the project's description and
images only, to show what the firmware meets on a strict machine. The plugin's failure report
covers the `machine` fixture, not that machine.
"""

import itertools

import pytest
from popcorn import Call, Machine, ModelError, Project, Return, StopReason
from popcorn.units import VirtualTime, ms
from rngmodel import CONVERSION, Rng, words

BUDGET = 2_000_000
pytestmark = pytest.mark.popcorn(budget=BUDGET)

COUNT = 8
RNG_NSCR_NIST_VALUE = 0x17CBB


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="rng_done", for_=ms(20))
    assert stop.reason is StopReason.ADDRESS, stop


def received(machine: Machine) -> list[int]:
    result = machine.var("rng_result")
    assert result.count.value == COUNT
    return [int(result.words[index].value) for index in range(COUNT)]


async def test_without_a_model_a_strict_run_names_the_rng(popcorn_project: Project) -> None:
    machine = Machine(
        popcorn_project.machine_description(),
        image=[image.load() for image in popcorn_project.images],
        strict_peripherals=True,
    )
    await popcorn_project.boot_machine(machine)
    with pytest.raises(ModelError, match="RNG"):
        await machine.run(for_=ms(20), budget=BUDGET)


async def test_the_firmware_gets_the_seeded_sequence(machine: Machine) -> None:
    await run_to_done(machine)
    assert received(machine) == list(itertools.islice(words(1), COUNT))

    model = machine.model("RNG")
    assert isinstance(model, Rng)
    assert model.served == COUNT


async def test_hal_rng_init_writes_nscr_into_the_description(
    machine: Machine, caplog: pytest.LogCaptureFixture
) -> None:
    await run_to_done(machine)
    assert machine.periph.RNG.NSCR.value == RNG_NSCR_NIST_VALUE
    assert not [r for r in caplog.records if "no register there" in r.getMessage()]


async def test_each_later_word_waits_one_conversion(machine: Machine) -> None:
    callbacks: list[VirtualTime] = []

    def record(call: Call) -> Return:
        callbacks.append(machine.time)
        return call.proceed()

    machine.intercept("HAL_RNG_ReadyDataCallback", record)
    await run_to_done(machine)
    assert len(callbacks) == COUNT
    for earlier, later in itertools.pairwise(callbacks):
        assert CONVERSION <= later - earlier < 2 * CONVERSION


@pytest.mark.popcorn(config="seeded", budget=BUDGET)
async def test_a_seed_from_the_project_file_changes_the_sequence(machine: Machine) -> None:
    await run_to_done(machine)
    assert received(machine) == list(itertools.islice(words(7), COUNT))
