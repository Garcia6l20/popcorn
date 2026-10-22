from __future__ import annotations

import subprocess
import sys
from collections.abc import Iterator

import popcorn
import pytest
from popcorn import ConfigError, ModelError, OptionBytes
from popcorn.model import Model
from popcorn.stm32 import PeripheralSupport, mcu, shipped_parts, support

PART = "STM32U575QGIx"
U575 = mcu(PART)
INSTANCES = [p.name for p in U575.peripherals if p.alias_of is None]

U575_BUILTIN = [
    "EXTI",
    "FLASH",
    "GPIOA",
    "GPIOB",
    "GPIOC",
    "GPIOD",
    "GPIOE",
    "GPIOF",
    "GPIOG",
    "GPIOH",
    "GPIOI",
    "I2C1",
    "I2C2",
    "I2C3",
    "I2C4",
    "ICache",
    "DCACHE",
    "IWDG",
    "PWR",
    "RCC",
    "RTC",
    "SAI1",
    "SAI2",
    "GPDMA1",
    "LPDMA1",
    "SPI1",
    "SPI2",
    "SPI3",
    "TIM1",
    "TIM8",
    "TIM2",
    "TIM3",
    "TIM4",
    "TIM5",
    "TIM15",
    "TIM16",
    "TIM17",
    "TIM6",
    "TIM7",
    "SYSCFG",
    "DBGMCU",
    "LPUART1",
    "CRC",
    "USART1",
    "USART2",
    "USART3",
    "UART4",
    "UART5",
    "HASH",
    "TAMP",
    "MDF1",
    "ADF1",
    "ADC1",
    "ADC12",
    "OTG_FS",
]
U575_TRUSTZONE_ONLY = [
    "GTZC1_MPCBB1",
    "GTZC1_MPCBB2",
    "GTZC1_MPCBB3",
    "GTZC1_TZIC",
    "GTZC1_TZSC",
    "GTZC2_MPCBB4",
    "GTZC2_TZIC",
    "GTZC2_TZSC",
]


class Rng(Model):
    pass


def modelled(machine: popcorn.Machine, name: str) -> bool:
    peripheral = next(p for p in machine.description.peripherals if p.name == name)
    if not peripheral.registers:
        pytest.skip(f"{name} has no register to read")
    try:
        machine.mem.u32[peripheral.base + peripheral.registers[0].offset]
    except ModelError:
        return False
    return True


@pytest.fixture(scope="module")
def machines() -> dict[bool, popcorn.Machine]:
    return {
        trustzone: popcorn.Machine(U575, option_bytes=OptionBytes(tzen=trustzone))
        for trustzone in (False, True)
    }


@pytest.fixture(scope="module")
def supported() -> dict[bool, dict[str, PeripheralSupport]]:
    return {
        trustzone: {s.name: s for s in support(U575, trustzone=trustzone)}
        for trustzone in (False, True)
    }


@pytest.fixture
def registered_rng() -> Iterator[None]:
    popcorn.model.register("RNG")(Rng)
    try:
        yield
    finally:
        popcorn.model.unregister("RNG")


def test_the_u575_ships() -> None:
    assert PART in shipped_parts()
    assert shipped_parts() == sorted(shipped_parts())


def test_armv8m_is_registered() -> None:
    assert "armv8m" in popcorn.arch_modules()


@pytest.mark.parametrize("trustzone", [False, True])
def test_support_lists_every_instance_in_description_order(trustzone: bool) -> None:
    found = support(U575, trustzone=trustzone)
    assert [s.name for s in found] == INSTANCES
    assert all(isinstance(s, PeripheralSupport) for s in found)
    rcc = next(s for s in found if s.name == "RCC")
    assert (rcc.ip, rcc.ip_version, rcc.model) == ("RCC", "STM32U5_rcc_v1_0", "builtin")


@pytest.mark.parametrize("trustzone", [False, True])
def test_the_u575_builtin_models_are_pinned(trustzone: bool) -> None:
    served = [s.name for s in support(U575, trustzone=trustzone) if s.model == "builtin"]
    expected = U575_BUILTIN + (U575_TRUSTZONE_ONLY if trustzone else [])
    assert sorted(served) == sorted(expected)


@pytest.mark.parametrize("trustzone", [False, True], ids=["plain", "trustzone"])
@pytest.mark.parametrize("name", INSTANCES)
def test_support_agrees_with_a_machine(
    machines: dict[bool, popcorn.Machine],
    supported: dict[bool, dict[str, PeripheralSupport]],
    trustzone: bool,
    name: str,
) -> None:
    found = supported[trustzone][name]
    assert (found.model is not None) == modelled(machines[trustzone], name)


@pytest.mark.usefixtures("registered_rng")
def test_a_registered_python_model_shows_as_python() -> None:
    found = {s.name: s.model for s in support(U575)}
    assert found["RNG"] == "python"
    assert isinstance(popcorn.Machine(U575).model("RNG"), Rng)


def test_a_python_model_over_a_builtin_ip_wins_until_unregistered() -> None:
    popcorn.model.register("CRC")(Rng)
    try:
        assert {s.name: s.model for s in support(U575)}["CRC"] == "python"
        assert isinstance(popcorn.Machine(U575).model("CRC"), Rng)
    finally:
        popcorn.model.unregister("CRC")
    assert {s.name: s.model for s in support(U575)}["CRC"] == "builtin"
    assert popcorn.Machine(U575).model("CRC") is None
    assert modelled(popcorn.Machine(U575), "CRC")


def test_without_a_registration_rng_has_no_model() -> None:
    assert {s.name: s.model for s in support(U575)}["RNG"] is None


def test_trustzone_on_a_part_without_it_is_refused(m33: popcorn.PartDescription) -> None:
    with pytest.raises(ConfigError) as caught:
        support(m33, trustzone=True)
    assert caught.value.key == "option_bytes.tzen"


UNREGISTERED_BUILTIN = """
import popcorn
from popcorn.model import Model
from popcorn.stm32 import mcu, support


class Crc(Model):
    pass


part = mcu("STM32U575QGIx")
popcorn.model.register("CRC", "integtest1_v2_0")(Crc)
popcorn.model.unregister("CRC", "integtest1_v2_0")
crc = next(p for p in part.peripherals if p.name == "CRC")
try:
    popcorn.Machine(part).mem.u32[crc.base]
except popcorn.ModelError:
    served = False
else:
    served = True
print({s.name: s.model for s in support(part)}["CRC"], served)
"""


def test_a_builtin_its_python_replacement_unregistered_shows_as_no_model() -> None:
    run = subprocess.run(
        [sys.executable, "-c", UNREGISTERED_BUILTIN], capture_output=True, text=True, check=True
    )
    assert run.stdout.split() == ["None", "False"]
