from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn import StopReason
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

# --8<-- [start:describe]
BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


# --8<-- [end:describe]


async def test_run_until_a_symbol(button: popcorn.Machine) -> None:
    # --8<-- [start:until]
    await button.run(for_=ms(1))
    button.periph.GPIOC.pin(13).level = 0
    stop = await button.run(until="HAL_GPIO_EXTI_Falling_Callback", for_=ms(1))
    assert stop.reason is StopReason.ADDRESS
    assert stop.location.symbol == "HAL_GPIO_EXTI_Falling_Callback"
    assert button.var("presses").value == 0
    # --8<-- [end:until]

    again = await button.run(until="HAL_GPIO_EXTI_Falling_Callback", for_=ms(1))
    assert again.reason is StopReason.TIME
    assert button.var("presses").value == 1


async def test_run_for_a_time(button: popcorn.Machine) -> None:
    # --8<-- [start:for]
    before = button.time
    stop = await button.run(for_=ms(5))
    assert stop.reason is StopReason.TIME
    assert stop.time - before >= ms(5)
    # --8<-- [end:for]


async def test_step(button: popcorn.Machine) -> None:
    # --8<-- [start:step]
    start = button.pc
    before = button.executed
    stop = await button.step(3)
    assert stop.reason is StopReason.BUDGET
    assert button.executed - before == 3
    assert button.pc != start
    # --8<-- [end:step]


async def test_a_budget_counts_instructions_not_sleep(button: popcorn.Machine) -> None:
    # --8<-- [start:budget]
    before = button.time
    executed = button.executed
    stop = await button.run(budget=1_000)
    assert stop.reason is StopReason.BUDGET
    assert button.executed - executed == 1_000
    assert stop.time - before > ms(10)
    # --8<-- [end:budget]


async def test_the_stop_says_where(button: popcorn.Machine) -> None:
    # --8<-- [start:stop]
    stop = await button.run(for_=ms(1))
    print(stop)
    print(stop.reason, hex(stop.pc), stop.location.file, stop.location.line)
    print(stop.instructions, stop.time)
    # --8<-- [end:stop]
    assert str(stop).startswith("TIME at 0x")
    assert stop.location.symbol == "main"
    assert stop.time.ps == stop.instructions * 250_000
    assert button.executed < 1_000 < stop.instructions
