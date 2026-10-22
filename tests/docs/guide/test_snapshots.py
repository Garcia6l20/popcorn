from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import ConfigError, Snapshot
from popcorn.pytest import MachineFactory
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


async def press(machine: popcorn.Machine) -> None:
    pin = machine.periph.GPIOC.pin(13)
    for level in (1, 0, 1):
        pin.level = level
        await machine.run(for_=ms(1))


async def test_a_snapshot_restores_in_place(button: popcorn.Machine) -> None:
    # --8<-- [start:restore]
    await press(button)
    snapshot = button.snapshot()
    assert button.var("presses").value == 1

    await press(button)
    assert button.var("presses").value == 2

    button.restore(snapshot)
    assert button.var("presses").value == 1
    # --8<-- [end:restore]


async def test_a_saved_snapshot_loads_into_another_machine(
    button: popcorn.Machine, machine_factory: MachineFactory, tmp_path: Path
) -> None:
    # --8<-- [start:save]
    await press(button)
    button.snapshot().save(tmp_path / "pressed.snap")

    other = await machine_factory.booted(BUTTON)
    other.load_snapshot(tmp_path / "pressed.snap")
    assert other.var("presses").value == 1
    assert other.time == button.time
    # --8<-- [end:save]


async def test_another_description_refuses_the_file(
    button: popcorn.Machine, machine_factory: MachineFactory, tmp_path: Path
) -> None:
    # --8<-- [start:refused]
    button.snapshot().save(tmp_path / "boot.snap")
    other = await machine_factory.booted(BUTTON.replace(faults="handler"))
    with pytest.raises(ConfigError) as refused:
        other.load_snapshot(tmp_path / "boot.snap")
    assert "config_digest" in str(refused.value)
    # --8<-- [end:refused]
    assert isinstance(Snapshot.load(tmp_path / "boot.snap"), Snapshot)


async def test_hooks_and_probes_survive_a_restore(button: popcorn.Machine) -> None:
    # --8<-- [start:hooks]
    pressed = button.probe("HAL_GPIO_EXTI_Falling_Callback")
    button.intercept("HAL_GPIO_TogglePin", lambda call: call.returns(None))
    snapshot = button.snapshot()
    await press(button)
    assert pressed.count == 1
    assert not button.periph.GPIOA.pin(5).level

    button.restore(snapshot)
    await press(button)
    assert pressed.count == 2
    assert not button.periph.GPIOA.pin(5).level
    # --8<-- [end:hooks]
