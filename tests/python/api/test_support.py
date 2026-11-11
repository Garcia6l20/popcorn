from __future__ import annotations

import json
import logging
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, ModelError, OptionBytes, _core
from popcorn.model import Model
from popcorn.stm32 import PeripheralSupport, mcu, shipped_parts, support

from popcorn_build import resolution_diff

PART = "STM32U575QGIx"
U575 = mcu(PART)
INSTANCES = [p.name for p in U575.peripherals if p.alias_of is None]

U575_BUILTIN = [
    "ADC4",
    "DAC1",
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
    "WWDG",
    "PWR",
    "RCC",
    "RNG",
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
    "LPTIM1",
    "LPTIM2",
    "LPTIM3",
    "LPTIM4",
    "LPUART1",
    "CRC",
    "CRS",
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
    "LPGPIO1",
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


class Stub(Model):
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
def registered_comp() -> Iterator[None]:
    popcorn.model.register("COMP")(Stub)
    try:
        yield
    finally:
        popcorn.model.unregister("COMP")


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


@pytest.mark.usefixtures("registered_comp")
def test_a_registered_python_model_shows_as_python() -> None:
    found = {s.name: s.model for s in support(U575)}
    assert found["COMP"] == "python"
    assert isinstance(popcorn.Machine(U575).model("COMP"), Stub)


def test_a_python_model_over_a_builtin_ip_wins_until_unregistered() -> None:
    popcorn.model.register("CRC")(Stub)
    try:
        assert {s.name: s.model for s in support(U575)}["CRC"] == "python"
        assert isinstance(popcorn.Machine(U575).model("CRC"), Stub)
    finally:
        popcorn.model.unregister("CRC")
    assert {s.name: s.model for s in support(U575)}["CRC"] == "builtin"
    assert popcorn.Machine(U575).model("CRC") is None
    assert modelled(popcorn.Machine(U575), "CRC")


def test_without_a_registration_comp_has_no_model() -> None:
    assert {s.name: s.model for s in support(U575)}["COMP"] is None


def test_trustzone_on_a_part_without_it_is_refused(m33: popcorn.PartDescription) -> None:
    with pytest.raises(ConfigError) as caught:
        support(m33, trustzone=True)
    assert caught.value.key == "option_bytes.tzen"


def test_trustzone_on_an_stm32l1_part_is_refused(m33_json: Path, tmp_path: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["part"] = "STM32L152RETx"
    raw["core"]["cpu"] = "CM3"
    path = tmp_path / "l1.json"
    path.write_text(json.dumps(raw))
    with pytest.raises(ConfigError) as caught:
        support(popcorn.load_description(path), trustzone=True)
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


ROW_PARTS = [
    "STM32U575QGIx",
    "STM32U575ZITxQ",
    "STM32H573IIKx",
    "STM32L152RETx",
    "STM32L476RGTx",
]


@pytest.mark.parametrize("part", ROW_PARTS)
def test_the_row_of_each_instance_is_the_golden(part: str) -> None:
    family = resolution_diff.family_of(part)
    golden = json.loads((resolution_diff.GOLDEN_DIR / f"{family}.json").read_text())
    expected = next(g["instances"] for g in golden["groups"] if part in g["parts"])
    found = {s.name: [s.row or "", s.source, s.variant or ""] for s in support(mcu(part))}
    assert found == {name: [entry[0], entry[1], entry[3]] for name, entry in expected.items()}


def test_a_row_names_the_instance_it_serves() -> None:
    found = {s.name: s for s in support(U575)}
    assert found["RCC"].row == "RCC/STM32U5_rcc_*/u5/*"
    assert found["RCC"].source == "row"
    assert found["GPIOA"].row == "GPIO/STM32U57x_gpio/u5/GPIO?"


def test_an_instance_no_row_lists_has_no_row() -> None:
    found = {s.name: s for s in support(U575)}
    assert found["COMP"].row is None
    assert found["COMP"].source == "none_no_row"
    assert found["OCTOSPIM"].row is None
    assert found["OCTOSPIM"].source == "none_no_row"
    assert found["LPGPIO1"].source == "row"
    assert found["LPGPIO1"].row == "GPIO/STM32U5x_gpio_*/u5/LPGPIO1"


def test_a_factory_that_declines_an_instance_keeps_its_row() -> None:
    plain = {s.name: s for s in support(U575)}
    secured = {s.name: s for s in support(U575, trustzone=True)}
    assert plain["GTZC1_TZSC"].model is None
    assert plain["GTZC1_TZSC"].row == secured["GTZC1_TZSC"].row == "GTZC/tzsc1_v2_2*/u5/*"
    assert secured["GTZC1_TZSC"].model == "builtin"


def test_a_part_no_row_of_the_version_serves_is_reported() -> None:
    other = mcu("STM32L152RETx")
    other.part = "STM32F407VGTx"
    found = {r.instance: r for r in _core.resolve_rows(other)}
    assert found["RCC"].source == "none_part_refused"
    assert found["RCC"].row is None
    assert found["RCC"].matches == []


def test_the_u575_gpio_matches_its_version_row_only() -> None:
    found = {r.instance: r for r in _core.resolve_rows(U575)}
    assert len(found["GPIOA"].matches) == 1
    assert len(found["RCC"].matches) == 1


def test_the_smps_pwr_matches_its_version_row_only() -> None:
    smps = {r.instance: r for r in _core.resolve_rows(mcu("STM32U575ZITxQ"))}
    assert smps["PWR"].row_label == "PWR/STM32U5_smps_pwr/u5/*"
    assert len(smps["PWR"].matches) == 1
    assert len({r.instance: r for r in _core.resolve_rows(U575)}["PWR"].matches) == 1


def test_explain_prints_one_line_per_instance() -> None:
    run = subprocess.run(
        [sys.executable, "-m", "popcorn.stm32", "explain", PART],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = run.stdout.splitlines()
    assert lines[0].split() == [
        "instance",
        "ip",
        "version",
        "model",
        "row",
        "source",
        "factory",
        "variant",
        "reads",
    ]
    assert [line.split()[0] for line in lines[1:]] == INSTANCES
    assert next(line for line in lines if line.startswith("RCC ")).split()[3:] == [
        "builtin",
        "RCC/STM32U5_rcc_*/u5/*",
        "row",
        "rcc_factory",
        "-",
        "rcc",
    ]


def test_explain_of_an_unknown_part_fails() -> None:
    run = subprocess.run(
        [sys.executable, "-m", "popcorn.stm32", "explain", "NOPE"], capture_output=True, text=True
    )
    assert run.returncode != 0
    assert "NOPE" in run.stderr


def test_explain_with_trustzone_lists_the_secured_models() -> None:
    run = subprocess.run(
        [sys.executable, "-m", "popcorn.stm32", "explain", PART, "--trustzone"],
        capture_output=True,
        text=True,
        check=True,
    )
    tzsc = next(line for line in run.stdout.splitlines() if line.startswith("GTZC1_TZSC "))
    assert tzsc.split()[3] == "builtin"


def test_a_machine_of_a_part_no_row_serves_refuses_the_first_instance() -> None:
    other = mcu("STM32L152RETx")
    other.part = "STM32F407VGTx"
    with pytest.raises(ConfigError) as caught:
        support(other)
    assert caught.value.key == "peripherals.CRC"
    assert "no model row of the CRC version integtest1_v1_0 serves the part STM32F407VGTx" in str(
        caught.value
    )


def answers_a_read(machine: popcorn.Machine, name: str) -> bool:
    peripheral = next(p for p in machine.description.peripherals if p.name == name)
    for register in peripheral.registers:
        try:
            machine.mem.u32[peripheral.base + register.offset]
        except ModelError:
            continue
        return True
    return False


@pytest.mark.parametrize("part", ["STM32H573IIKx", "STM32L152RETx", "STM32L476RGTx"])
def test_a_machine_models_exactly_the_instances_a_row_serves(part: str) -> None:
    description = mcu(part)
    machine = popcorn.Machine(description)
    mismatched = [
        s.name
        for s in support(description)
        if next(p for p in description.peripherals if p.name == s.name).registers
        and s.ip != "GTZC"
        and (s.row is not None) != answers_a_read(machine, s.name)
    ]
    assert mismatched == []


def test_a_flash_row_names_its_variant_and_the_member_it_reads() -> None:
    found = {s.name: s for s in support(U575)}
    assert found["FLASH"].variant == "flash_u5"
    assert {r.instance: r for r in _core.resolve_rows(U575)}["FLASH"].reads == "flash"
    l1 = {s.name: s for s in support(mcu("STM32L152RETx"))}
    assert l1["FLASH"].variant == "l1_flash_cat5"


def test_option_bytes_on_a_part_no_flash_row_serves_name_the_flash_row() -> None:
    other = mcu(PART)
    other.part = "STM32X575ZITxQ"
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(other, option_bytes=OptionBytes())
    assert caught.value.key == "peripherals.FLASH"


def test_a_row_reading_two_board_members_names_both() -> None:
    found = {r.instance: r for r in _core.resolve_rows(U575)}
    assert found["SPI1"].reads == "spi,spi_controllers"
    assert found["I2C1"].reads == "i2c,i2c_controllers"
    assert found["USART1"].reads == "uarts"


def h503_hash_only() -> popcorn.PartDescription:
    try:
        description = mcu("STM32H503RBTx")
    except popcorn.MissingInput as exc:
        pytest.skip(f"STM32H503RBTx sources not found: {exc}")
    description.peripherals = [p for p in description.peripherals if p.name == "HASH"]
    return description


def test_the_h503_hash_is_served_by_the_u5_model_row() -> None:
    found = {s.name: s for s in support(h503_hash_only())}
    assert found["HASH"].row == "HASH/hash1_v4*/h5:STM32H50*/*"
    assert found["HASH"].model == "builtin"


def test_the_h503_hash_stores_00_for_the_reserved_algo_01(caplog: pytest.LogCaptureFixture) -> None:
    description = h503_hash_only()
    base = next(p.base for p in description.peripherals if p.name == "HASH")
    m = popcorn.Machine(description)
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m.mem.u32[base] = 1 << 17
    assert m.mem.u32[base] == 0
    assert [r.getMessage() for r in caplog.records if "ALGO 0x1" in r.getMessage()]
    m.mem.u32[base] = 3 << 17
    assert m.mem.u32[base] == 3 << 17


def test_the_h503_hash_refuses_the_reserved_algo_01_under_strict() -> None:
    description = h503_hash_only()
    base = next(p.base for p in description.peripherals if p.name == "HASH")
    m = popcorn.Machine(description, strict_peripherals=True)
    with pytest.raises(ModelError):
        m.mem.u32[base] = 1 << 17
    assert m.mem.u32[base] == 0
