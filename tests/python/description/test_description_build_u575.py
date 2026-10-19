import json
import logging
from dataclasses import replace
from pathlib import Path
from typing import cast

import jsonschema.validators
import pytest
from popcorn.stm32.description._binder import dump_json, load_json
from popcorn.stm32.description._builder import (
    bare_register_names,
    build_description,
    resolve_ip,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    AlternateDesc,
    CrossEffect,
    CrossRule,
    PeripheralDesc,
    RegisterDesc,
)
from popcorn.stm32.description._sources import Sources

SCHEMA_PATH = Path(__file__).parents[3] / "data" / "schema" / "part_description.schema.json"


def _schema() -> dict[str, object]:
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return cast(dict[str, object], data)


def test_build_u575_matches_expected_shape(u575_sources: Sources) -> None:
    description = build_description(u575_sources)

    assert len(description.peripherals) == 197
    assert sum(1 for p in description.peripherals if p.alias_of is not None) == 98
    assert len(description.regions) == 14
    assert len(description.irqs) == 121

    assert description.sources["svd"]
    assert description.sources["mcu_db"]
    assert description.sources["rzone"]
    assert description.sources["cmsis_header"]
    assert description.sources["patch:stm32u5.toml"]
    assert description.sources["patch:stm32u575.toml"]
    assert description.sources["gpio_modes"]


def test_build_u575_gpio_ports_carry_their_alternate_functions(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    by_name = {p.name: p for p in description.peripherals}
    porta = by_name["GPIOA"].alternates
    assert porta is not None
    assert AlternateDesc(pin=6, af=2, signal="tim3_ch1") in porta
    assert AlternateDesc(pin=0, af=1, signal="tim2_ch1") in porta
    assert AlternateDesc(pin=0, af=14, signal="tim2_etr") in porta
    assert porta == sorted(porta, key=lambda a: (a.pin, a.af, a.signal))
    assert by_name["GPIOH"].alternates == []
    assert all(
        p.alternates is None
        for p in description.peripherals
        if not p.name.startswith("GPIO") or p.alias_of is not None
    )


def test_a_modes_file_without_af_numbers_logs_and_carries_none(
    u575_sources: Sources, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    modes = tmp_path / "GPIO-afio_Modes.xml"
    modes.write_bytes(
        b'<IP xmlns="http://mcd.rou.st.com/modules.php?name=mcu"><GPIO_Pin Name="PA6">'
        b'<PinSignal Name="TIM3_CH1"><SpecificParameter Name="GPIO_AF">'
        b"<PossibleValue>__HAL_AFIO_REMAP_TIM3_DISABLE</PossibleValue>"
        b"</SpecificParameter></PinSignal></GPIO_Pin></IP>"
    )
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        description = build_description(replace(u575_sources, gpio_modes=modes))
    assert all(p.alternates is None for p in description.peripherals)
    assert any("AFIO" in record.message for record in caplog.records)


def test_build_without_a_gpio_modes_file_logs_and_carries_none(
    u575_sources: Sources, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        description = build_description(replace(u575_sources, gpio_modes=None))
    assert all(p.alternates is None for p in description.peripherals)
    assert "gpio_modes" not in description.sources
    assert any("no CubeMX GPIO modes file" in record.message for record in caplog.records)


def test_build_u575_validates_against_schema(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    text = dump_json(description)
    payload = json.loads(text)

    schema = _schema()
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator_cls(schema).validate(payload)


def test_build_u575_round_trips_through_load_json(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    text = dump_json(description)
    reloaded = load_json(text, file="in-memory")
    assert reloaded == description


def test_build_u575_from_open_pin_data_namespaced_copies_has_the_same_content(
    u575_sources: Sources, tmp_path: Path
) -> None:
    assert u575_sources.gpio_modes is not None
    copies: dict[str, Path] = {}
    for role, source in (("mcu_db", u575_sources.mcu_db), ("gpio_modes", u575_sources.gpio_modes)):
        data = source.read_bytes()
        assert data.count(b'xmlns="http://mcd.rou.st.com/modules.php?name=mcu"') == 1
        copies[role] = tmp_path / source.name
        copies[role].write_bytes(
            data.replace(b"http://mcd.rou.st.com/modules.php?name=mcu", b"http://dummy.com")
        )
    opened = build_description(
        replace(u575_sources, mcu_db=copies["mcu_db"], gpio_modes=copies["gpio_modes"])
    )
    local = build_description(u575_sources)
    assert opened.sources["mcu_db"] != local.sources["mcu_db"]

    def content(text: str) -> dict[str, object]:
        data = cast(dict[str, object], json.loads(text))
        del data["generator"], data["sources"]
        return data

    assert content(dump_json(opened)) == content(dump_json(local))


def test_a_build_with_neither_rzone_nor_facts_is_a_description_error(
    u575_sources: Sources,
) -> None:
    with pytest.raises(DescriptionError) as excinfo:
        build_description(replace(u575_sources, rzone=None))
    assert excinfo.value.key == "rzone"


def test_build_u575_adc12_ip_avoids_collision(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    by_name = {p.name: p for p in description.peripherals}
    assert by_name["ADC12"].ip == "ADC12"


def test_resolve_ip_avoids_synthetic_groupname_collision() -> None:
    real_ip_names = {"FOO"}

    ip, ip_version = resolve_ip("FOO12_COMMON", "FOO", None, None, real_ip_names)
    assert ip == "FOO12_COMMON"
    assert ip_version == ""


def test_resolve_ip_keeps_group_fallback_when_no_collision() -> None:
    real_ip_names = {"BAR"}

    ip, ip_version = resolve_ip("BAZ_COMMON", "BAZGROUP", None, None, real_ip_names)
    assert ip == "BAZGROUP"
    assert ip_version == ""


def test_resolve_ip_prefers_matched_db_ip() -> None:
    ip, ip_version = resolve_ip("FOO1", "FOO", "FOO", "1.0", {"FOO"})
    assert ip == "FOO"
    assert ip_version == "1.0"


BARE_NAMES = {
    "EXTI": (14, "EXTI_"),
    "FLASH": (62, "FLASH_"),
    "GPIOA": (13, "GPIO_"),
    "GPIOB": (13, "GPIO_"),
    "GPIOC": (13, "GPIO_"),
    "GPIOD": (13, "GPIO_"),
    "GPIOE": (13, "GPIO_"),
    "GPIOF": (13, "GPIO_"),
    "GPIOG": (13, "GPIO_"),
    "GPIOH": (13, "GPIO_"),
    "GPIOI": (13, "GPIO_"),
    "I2C1": (1, "I2C_"),
    "I2C2": (1, "I2C_"),
    "I2C3": (1, "I2C_"),
    "I2C4": (1, "I2C_"),
    "ICache": (10, "ICACHE_"),
    "DCACHE": (10, "DCACHE_"),
    "GTZC1_MPCBB1": (107, "MPCBB1_"),
    "GTZC1_MPCBB2": (107, "MPCBB2_"),
    "GTZC1_MPCBB3": (107, "MPCBB3_"),
    "GTZC1_TZSC": (23, "TZSC_"),
    "GTZC2_MPCBB4": (4, "MPCBB4_"),
    "GTZC2_TZSC": (3, "TZSC_"),
    "PWR": (38, "PWR_"),
    "RCC": (52, "RCC_"),
    "GPDMA1": (173, "GPDMA_"),
    "LPDMA1": (45, "LPDMA_"),
    "SPI1": (14, "SPI_"),
    "SPI2": (14, "SPI_"),
    "SPI3": (14, "SPI_"),
    "TIM1": (30, "TIM1_"),
    "TIM8": (30, "TIM1_"),
    "SYSCFG": (13, "SYSCFG_"),
    "DBGMCU": (3, "DBGMCU_"),
    "VREFBUF": (2, "VREFBUF_"),
    "UCPD1": (15, "UCPD_"),
    "FDCAN1_RAM": (37, "FDCAN_"),
    "FDCAN1": (37, "FDCAN_"),
    "TAMP": (49, "TAMP_"),
    "MDF1": (96, "MDF_"),
    "ADF1": (15, "ADF_"),
    "LPGPIO1": (5, "LPGPIO_"),
    "ADC1": (34, "ADC_"),
    "ADC4": (18, "ADC_"),
    "ADC12": (1, "ADC12_"),
    "DAC1": (21, "DAC_"),
    "SDMMC1": (5, "SDMMC_"),
    "SDMMC2": (5, "SDMMC_"),
    "DLYBOS1": (2, "DLYB_"),
    "DLYBOS2": (2, "DLYB_"),
    "DLYBSD1": (2, "DLYB_"),
    "DLYBSD2": (2, "DLYB_"),
}


def test_build_u575_strips_the_peripheral_prefix_of_register_names(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    by_name = {p.name: p for p in description.peripherals}
    renamed: dict[str, tuple[int, str]] = {}
    for peripheral in description.peripherals:
        aliased = [r for r in peripheral.registers if r.aliases]
        if aliased and peripheral.alias_of is None:
            prefixes = {r.aliases[0].removesuffix(r.name) for r in aliased if r.aliases}
            assert len(prefixes) == 1, peripheral.name
            renamed[peripheral.name] = (len(aliased), prefixes.pop())
        if peripheral.alias_of is not None:
            target = by_name[peripheral.alias_of]
            assert [(r.name, r.aliases) for r in peripheral.registers] == [
                (r.name, r.aliases) for r in target.registers
            ]
    assert renamed == BARE_NAMES
    assert [r.name for r in by_name["COMP"].registers] == ["COMP1_CSR", "COMP2_CSR"]
    assert [r.name for r in by_name["OPAMP"].registers][:2] == ["OPAMP1_CSR", "OPAMP1_OTR"]


def _register(name: str, offset: int, alternate_of: str | None = None) -> RegisterDesc:
    return RegisterDesc(
        name=name,
        offset=offset,
        width=32,
        reset=0,
        reset_mask=0,
        acc=Access.READ_WRITE,
        fields=[],
        alternate_of=alternate_of,
    )


def _peripheral(name: str, ip: str, registers: list[RegisterDesc]) -> PeripheralDesc:
    return PeripheralDesc(
        name=name,
        ip=ip,
        ip_version="",
        base=0,
        size=0x400,
        registers=registers,
        cross_rules=[],
        irqs=[],
    )


def test_bare_names_follow_alternates_and_cross_rules() -> None:
    timer = _peripheral(
        "TIM8",
        "TIM1_8U5",
        [
            _register("TIM1_CCMR1_Output", 0x18),
            _register("TIM1_CCMR1_Input", 0x18, "TIM1_CCMR1_Output"),
            _register("TIM1_SR", 0x10),
            _register("TIM1_EGR", 0x14),
        ],
    )
    timer.cross_rules = [
        CrossRule(
            reg="TIM1_EGR",
            field="UG",
            target_reg="TIM1_SR",
            target_field="UIF",
            effect=CrossEffect.SET,
        )
    ]
    renames = bare_register_names(timer, ["TIM1"])
    assert renames["TIM1_SR"] == "SR"
    assert [(r.name, r.aliases, r.alternate_of) for r in timer.registers] == [
        ("CCMR1_Output", ["TIM1_CCMR1_Output"], None),
        ("CCMR1_Input", ["TIM1_CCMR1_Input"], "CCMR1_Output"),
        ("SR", ["TIM1_SR"], None),
        ("EGR", ["TIM1_EGR"], None),
    ]
    assert (timer.cross_rules[0].reg, timer.cross_rules[0].target_reg) == ("EGR", "SR")


def test_a_bare_name_that_collides_keeps_the_svd_name() -> None:
    comp = _peripheral("COMP", "COMP", [_register("COMP1_CSR", 0), _register("COMP2_CSR", 4)])
    assert bare_register_names(comp, []) == {}
    clash = _peripheral(
        "UNIT", "UNIT", [_register("CR", 0), _register("UNIT_CR", 4), _register("UNIT_SR", 8)]
    )
    assert bare_register_names(clash, []) == {"UNIT_SR": "SR"}
    assert [r.name for r in clash.registers] == ["CR", "UNIT_CR", "SR"]
    twice = _peripheral("ADC1", "ADC", [_register("ADC1_ISR", 0), _register("ADC_ISR", 4)])
    assert bare_register_names(twice, []) == {"ADC1_ISR": "ISR"}
    digit = _peripheral("UNIT", "UNIT", [_register("UNIT_1CR", 0)])
    assert bare_register_names(digit, []) == {}
