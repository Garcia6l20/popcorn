from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn import Call, Returned, StopReason, WatchKind
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)

PC13 = 1 << 13


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


async def settle(machine: popcorn.Machine) -> None:
    await machine.run(for_=ms(1))


async def press(machine: popcorn.Machine) -> None:
    pin = machine.periph.GPIOC.pin(13)
    for level in (1, 0, 1):
        pin.level = level
        await machine.run(for_=ms(1))


async def test_an_intercept_sees_the_arguments(button: popcorn.Machine) -> None:
    # --8<-- [start:intercept]
    await settle(button)

    @button.intercept("HAL_GPIO_EXTI_Falling_Callback")
    def on_press(call: Call) -> popcorn.Return:
        assert call.arg(0) == PC13
        return call.proceed()

    await press(button)
    log = button.intercept_log("HAL_GPIO_EXTI_Falling_Callback")
    assert [entry.args for entry in log] == [(PC13,)]
    # --8<-- [end:intercept]


async def test_an_intercept_replaces_a_function(button: popcorn.Machine) -> None:
    # --8<-- [start:replace]
    await settle(button)
    button.intercept("HAL_GPIO_TogglePin", lambda call: call.returns(None))
    await press(button)
    assert button.var("presses").value == 1
    assert not button.periph.GPIOA.pin(5).level
    # --8<-- [end:replace]


async def test_a_script_answers_the_first_calls(button: popcorn.Machine) -> None:
    # --8<-- [start:script]
    button.script("HAL_RCC_GetSysClockFreq", [100, 200])
    assert (await button.call("HAL_RCC_GetSysClockFreq")).value == 100
    assert (await button.call("HAL_RCC_GetSysClockFreq")).value == 200
    assert (await button.call("HAL_RCC_GetSysClockFreq")).value == 4_000_000
    # --8<-- [end:script]


async def test_a_return_hook_sees_the_value(button: popcorn.Machine) -> None:
    # --8<-- [start:return]
    returns: list[Returned] = []
    button.on_return("HAL_GPIO_EXTI_Falling_Callback", returns.append)
    await settle(button)
    await press(button)
    assert [r.args for r in returns] == [(PC13,)]
    assert returns[0].value is None
    assert returns[0].instructions > 0
    # --8<-- [end:return]


async def test_a_watch_sees_every_write(button: popcorn.Machine) -> None:
    # --8<-- [start:watch]
    await settle(button)
    watch = button.watch("presses", size=4, kind=WatchKind.WRITE)
    await press(button)
    assert [hit.value for hit in watch.hits] == [1]
    assert watch.hits[0].write
    # --8<-- [end:watch]


async def test_a_watch_without_a_handler_stops_the_run(button: popcorn.Machine) -> None:
    # --8<-- [start:watch_stop]
    await settle(button)
    button.watch("presses", size=4, kind=WatchKind.WRITE)
    button.periph.GPIOC.pin(13).level = 0
    stop = await button.run(for_=ms(5))
    assert stop.reason is StopReason.WATCHPOINT
    # --8<-- [end:watch_stop]


async def test_a_probe_counts_arrivals(button: popcorn.Machine) -> None:
    # --8<-- [start:probe]
    await settle(button)
    pressed = button.probe("HAL_GPIO_EXTI_Falling_Callback")
    button.periph.GPIOC.pin(13).level = 0
    assert await pressed.hit(within=ms(5))
    assert pressed.count == 1
    assert pressed.first_at is not None
    # --8<-- [end:probe]


async def test_a_condition_ends_a_wait(button: popcorn.Machine) -> None:
    # --8<-- [start:condition]
    await settle(button)
    presses = button.ref.var("presses")
    button.periph.GPIOC.pin(13).level = 0
    stop = await button.wait_for(presses.eq(1), for_=ms(5))
    assert stop.reason is StopReason.USER
    assert stop.fired
    # --8<-- [end:condition]


async def test_a_predicate_ends_a_wait(button: popcorn.Machine) -> None:
    # --8<-- [start:predicate]
    await settle(button)
    button.periph.GPIOC.pin(13).level = 0
    held = await button.wait_until(lambda m: m.var("presses").value == 1, for_=ms(5))
    assert held is not None
    # --8<-- [end:predicate]


async def test_a_host_can_call_a_firmware_function(button: popcorn.Machine) -> None:
    # --8<-- [start:call]
    assert (await button.call("HAL_RCC_GetSysClockFreq")).value == 4_000_000
    # --8<-- [end:call]


async def test_a_recording_keeps_returns_with_their_times(button: popcorn.Machine) -> None:
    # --8<-- [start:record]
    presses = button.record_returns("HAL_GPIO_EXTI_Falling_Callback", lambda r: r.args[0])
    await settle(button)
    mark = presses.mark()
    await press(button)
    seen = presses.since(mark)
    assert [entry.value for entry in seen] == [PC13]
    assert seen[0].at <= button.time
    # --8<-- [end:record]
