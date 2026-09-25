from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import read_part
from popcorn.stm32.description._model import ReadEffect, WriteEffect
from popcorn.stm32.description._patches import PatchReport, PatchTarget, apply_patches, load_patches
from popcorn.stm32.description._svd import expand_svd

from .conftest import target_from_svd, target_peripheral

type _TargetAndReport = tuple[PatchTarget, PatchReport]

STM32U5_TOML = (
    Path(__file__).parents[3]
    / "python"
    / "popcorn"
    / "stm32"
    / "description"
    / "patches"
    / "stm32u5.toml"
)

_IN_PLACE_REGISTERS = [
    ("ADC1", "ADC_ISR"),
    ("EXTI", "EXTI_RPR1"),
    ("EXTI", "EXTI_FPR1"),
    ("FLASH", "FLASH_NSSR"),
    ("FLASH", "FLASH_SECSR"),
    ("I2C1", "ISR"),
    ("I2C2", "ISR"),
    ("TIM2", "SR"),
]

_CLEAR_REGISTERS = [
    *[("GPDMA1", f"GPDMA_C{n}FCR") for n in range(16)],
    ("I2C1", "ICR"),
    ("I2C2", "ICR"),
    ("RCC", "RCC_CICR"),
    ("RTC", "SCR"),
    ("SAI1", "ACLRFR"),
    ("SAI1", "BCLRFR"),
    ("SAI2", "ACLRFR"),
    ("SAI2", "BCLRFR"),
    ("SPI1", "SPI_IFCR"),
    ("UART4", "ICR"),
    ("USART1", "ICR"),
    ("USART2", "ICR"),
    ("USART3", "ICR"),
    ("UCPD1", "UCPD_ICR"),
]


@pytest.fixture(scope="session")
def u575_target_and_report(st_svd_u575: Path, st_cubemx_db: Path) -> _TargetAndReport:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    part = read_part(
        (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes(), file="STM32U575QGIx.xml"
    )
    db_ips = {ip.instance: ip for ip in part.ips}
    target = target_from_svd(device, db_ips)

    patches = load_patches([STM32U5_TOML], part="STM32U575QGIx", svd_name="STM32U575.svd")
    report = apply_patches(patches, target)
    return target, report


def test_every_in_place_register_has_a_rule(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    for peripheral_name, register_name in _IN_PLACE_REGISTERS:
        peripheral = target_peripheral(target, peripheral_name)
        register = next(r for r in peripheral.registers if r.name == register_name)
        touched = any(
            f.on_write != WriteEffect.MODIFY or f.on_read != ReadEffect.NONE
            for f in register.fields
        )
        assert touched, f"{peripheral_name}.{register_name} has no patched field"


def test_every_clear_register_appears_as_a_cross_rule_source(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    for peripheral_name, register_name in _CLEAR_REGISTERS:
        peripheral = target_peripheral(target, peripheral_name)
        assert any(
            c.reg == register_name for c in peripheral.cross_rules
        ), f"{peripheral_name}.{register_name} has no cross rule"


def test_usart1_tccf_clears_tc_the_p1_toy_rule(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    usart1 = target_peripheral(target, "USART1")
    tccf = next(c for c in usart1.cross_rules if c.field == "TCCF")
    assert tccf.reg == "ICR"
    assert tccf.target_reg == "ISR_enabled"
    assert tccf.target_field == "TC"
    assert tccf.effect.value == "clear"


def test_uart4_and_lpuart1_have_their_own_cross_rules_not_the_usart_one(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    uart4 = target_peripheral(target, "UART4")
    uart4_tccf = next(c for c in uart4.cross_rules if c.field == "TCCF")
    assert uart4_tccf.target_reg == "ISR_enabled"
    assert uart4_tccf.target_field == "TC"

    lpuart1 = target_peripheral(target, "LPUART1")
    lpuart1_tccf = next(c for c in lpuart1.cross_rules if c.field == "TCCF")
    assert lpuart1_tccf.target_reg == "ISR"
    assert lpuart1_tccf.target_field == "TC"
    assert len(lpuart1.cross_rules) == 8


def test_gpdma1_has_cross_rules_for_all_16_channels(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    gpdma1 = target_peripheral(target, "GPDMA1")
    for n in range(16):
        reg = f"GPDMA_C{n}FCR"
        target_reg = f"GPDMA_C{n}SR"
        matching = [c for c in gpdma1.cross_rules if c.reg == reg]
        assert matching, f"no cross rule for channel {n}"
        assert all(c.target_reg == target_reg for c in matching)


def test_tim2_uif_and_cc2if_zero_to_clear(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    tim2 = target_peripheral(target, "TIM2")
    sr = next(r for r in tim2.registers if r.name == "SR")
    uif = next(f for f in sr.fields if f.name == "UIF")
    cc2if = next(f for f in sr.fields if f.name == "CC2IF")
    assert uif.on_write.value == "zero_to_clear"
    assert cc2if.on_write.value == "zero_to_clear"


def test_every_tim_instance_gets_its_uif_rule(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    for name in ("TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM8", "TIM15", "TIM16", "TIM17"):
        peripheral = target_peripheral(target, name)
        sr_name = "TIM1_SR" if name in ("TIM1", "TIM8") else "SR"
        sr = next(r for r in peripheral.registers if r.name == sr_name)
        uif = next(f for f in sr.fields if f.name == "UIF")
        assert uif.on_write.value == "zero_to_clear", name


def test_tim16_sr_every_field_zero_to_clear_and_no_cc2if(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    tim16 = target_peripheral(target, "TIM16")
    sr = next(r for r in tim16.registers if r.name == "SR")
    by_name = {f.name: f for f in sr.fields}
    assert set(by_name) == {"UIF", "CC1IF", "COMIF", "BIF", "CC1OF"}
    assert all(f.on_write.value == "zero_to_clear" for f in by_name.values())


def test_adc4_eocal_and_ldordy_one_to_clear(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    adc4 = target_peripheral(target, "ADC4")
    isr = next(r for r in adc4.registers if r.name == "ADC_ISR")
    eocal = next(f for f in isr.fields if f.name == "EOCAL")
    ldordy = next(f for f in isr.fields if f.name == "LDORDY")
    assert eocal.on_write.value == "one_to_clear"
    assert ldordy.on_write.value == "one_to_clear"


def test_exti_rpr1_fields_one_to_clear(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    exti = target_peripheral(target, "EXTI")
    rpr1 = next(r for r in exti.registers if r.name == "EXTI_RPR1")
    assert all(f.on_write.value == "one_to_clear" for f in rpr1.fields)
    assert len(rpr1.fields) == 26


def test_reset_clock_hz_is_4mhz(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    assert target.core.reset_clock_hz == 4_000_000


def test_sec_usart1_carries_the_same_rules_as_usart1(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    usart1 = target_peripheral(target, "USART1")
    sec_usart1 = target_peripheral(target, "SEC_USART1")
    assert sec_usart1.alias_of == "USART1"
    assert sec_usart1.ip == usart1.ip
    assert {(c.reg, c.field, c.target_reg, c.target_field) for c in sec_usart1.cross_rules} == {
        (c.reg, c.field, c.target_reg, c.target_field) for c in usart1.cross_rules
    }


def test_report_totals(u575_target_and_report: _TargetAndReport) -> None:
    _, report = u575_target_and_report
    assert report.ip_renamed == 26
    assert report.aliased == 98
    assert report.fields_touched == 217
    assert report.cross_rules_added == 570
    assert report.irqs_dropped == 0
    assert report.irqs_added == 1
    assert report.core_fields_set == 1
