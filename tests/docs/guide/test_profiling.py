from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/tim/prebuilt/tim_ticks.elf"

TICKS = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def ticks() -> popcorn.MachineDescription:
    return TICKS


async def test_a_profile_reports_the_sleeps_and_the_timer_interrupt(ticks: popcorn.Machine) -> None:
    # --8<-- [start:profile]
    with ticks.profile("sleep", "irq") as profile:
        await ticks.run(for_=ms(100))
    report = profile.report()
    print(report.text())
    # --8<-- [end:profile]

    assert report.sleep is not None
    assert report.sleep.share > 0.99
    assert report.irq is not None
    assert report.irq.exceptions[0].name == "TIM2"
