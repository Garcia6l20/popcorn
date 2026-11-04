"""A push button on PC13 toggles an LED on PA5 through an EXTI falling-edge interrupt, HAL, on
the STM32L476RCTx under strict.

The host plays the button: it drives PC13 low to press and high to release, through the GPIO
pin view, and reads PA5 back the same way. `HAL_GPIO_Init` routes EXTI line 13 to port C in
SYSCFG_EXTICR4, and the line shares EXTI15_10 with lines 10 to 15. The firmware sleeps in WFI
between presses: a full trace of the translated blocks shows a few blocks per SysTick wake-up,
not a busy loop.
"""

from pathlib import Path

import pytest
from popcorn import Machine, MachineDescription, StopReason
from popcorn.pytest import MachineFactory
from popcorn.trace import Block, Irq, TraceKind, read
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=1_000_000)

SYSTICK = 15
EXTI15_10 = 16 + 40
EXTICR_PORT_C = 2
MAX_BLOCKS_PER_TICK = 10


async def settle(machine: Machine) -> None:
    stop = await machine.run(for_=ms(1))
    assert stop.reason is StopReason.TIME, stop


async def press(machine: Machine) -> None:
    button = machine.periph.GPIOC.pin(13)
    button.level = 0
    await settle(machine)
    button.level = 1
    await settle(machine)


async def test_the_button_rests_high_on_its_pull_up_and_the_led_starts_off(
    gpio: Machine,
) -> None:
    await settle(gpio)
    assert gpio.periph.GPIOC.pin(13).level
    assert not gpio.periph.GPIOA.pin(5).level
    assert gpio.var("presses").value == 0


async def test_line_13_is_routed_to_port_c_on_a_falling_edge(gpio: Machine) -> None:
    await settle(gpio)
    assert gpio.periph.SYSCFG.EXTICR4.EXTI13 == EXTICR_PORT_C
    exti = gpio.periph.EXTI
    line = 1 << 13
    assert (exti.IMR1.value & line, exti.FTSR1.value & line, exti.RTSR1.value & line) == (
        line,
        line,
        0,
    )


async def test_each_press_toggles_the_led(gpio: Machine) -> None:
    await settle(gpio)
    led = gpio.periph.GPIOA.pin(5)
    seen: list[bool] = []
    for _ in range(3):
        await press(gpio)
        seen.append(led.level)
    assert seen == [True, False, True]
    assert gpio.var("presses").value == 3


async def test_driving_the_button_high_and_releasing_it_is_no_press(gpio: Machine) -> None:
    await settle(gpio)
    button = gpio.periph.GPIOC.pin(13)
    button.level = 1
    await settle(gpio)
    button.release()
    await settle(gpio)
    assert gpio.var("presses").value == 0


async def test_the_firmware_sleeps_between_presses(gpio: Machine, tmp_path: Path) -> None:
    await press(gpio)
    gpio.ring.clear()
    with gpio.trace([TraceKind.BLOCK], tmp_path / "idle.pctr"):
        stop = await gpio.run(for_=ms(10))
    assert stop.reason is StopReason.TIME, stop

    entries = [r.number for r in gpio.ring.records() if isinstance(r, Irq) and r.enter]
    assert entries == [SYSTICK] * 10
    blocks = [r for r in read(tmp_path / "idle.pctr") if isinstance(r, Block)]
    assert len(blocks) <= MAX_BLOCKS_PER_TICK * len(entries)


async def test_a_press_runs_the_exti15_10_handler_once(gpio: Machine) -> None:
    await settle(gpio)
    gpio.ring.clear()
    await press(gpio)
    entries = [r.number for r in gpio.ring.records() if isinstance(r, Irq) and r.enter]
    assert entries.count(EXTI15_10) == 1


async def test_a_second_machine_from_the_boot_snapshot_takes_presses(
    machine_factory: MachineFactory, gpio_description: MachineDescription
) -> None:
    second = await machine_factory.booted(gpio_description)
    await settle(second)
    await press(second)
    assert second.var("presses").value == 1
    assert second.periph.GPIOA.pin(5).level


async def test_the_led_toggles_on_the_falling_edge_while_the_button_is_held(
    gpio: Machine,
) -> None:
    await settle(gpio)
    button = gpio.periph.GPIOC.pin(13)
    led = gpio.periph.GPIOA.pin(5)
    button.level = 0
    await settle(gpio)
    assert (led.level, gpio.var("presses").value) == (True, 1)
    button.level = 1
    await settle(gpio)
    assert (led.level, gpio.var("presses").value) == (True, 1)
