from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import read_part, read_rzone
from popcorn.stm32.description._model import (
    Access,
    ReadEffect,
    RegionAccess,
    RegionKind,
    WriteEffect,
)
from popcorn.stm32.description._patches import PatchReport, PatchTarget, apply_patches, load_patches
from popcorn.stm32.description._sources import Sources
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
def u575_target_and_report(
    u575_sources: Sources, st_svd_u575: Path, st_cubemx_db: Path
) -> _TargetAndReport:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    part = read_part(
        (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes(), file="STM32U575QGIx.xml"
    )
    db_ips = {ip.instance: ip for ip in part.ips}
    target = target_from_svd(device, db_ips)
    target.regions = read_rzone(
        u575_sources.rzone.read_bytes(), file=str(u575_sources.rzone)
    ).regions
    target.db_part = part

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


def test_tim15_cr2_mms_is_three_bits_wide(u575_target_and_report: _TargetAndReport) -> None:
    target, _ = u575_target_and_report
    cr2 = next(r for r in target_peripheral(target, "TIM15").registers if r.name == "CR2")
    assert [(f.name, f.lsb, f.width) for f in cr2.fields if f.name in ("MMS", "TI1S")] == [
        ("TI1S", 7, 1),
        ("MMS", 4, 3),
    ]


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
    assert report.fields_touched == 218
    assert report.cross_rules_added == 570
    assert report.irqs_dropped == 0
    assert report.irqs_added == 1
    assert report.core_fields_set == 1
    assert report.sizes_set == 1
    assert report.registers_added == 13
    assert report.regions_added == 4
    assert report.controllers_set == 2
    assert report.signature_words == 6


_ARCH_WINDOWS = (0xE000_0000, 0xE000_1000, 0xE000_2000, 0xE000_E000)


def test_no_peripheral_overlaps_an_arch_module_window(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, report = u575_target_and_report
    names = {p.name for p in target.peripherals}
    assert "DCB" not in names
    assert "DBGMCU" in names
    assert report.peripherals_dropped == 1
    overlapping = [
        p.name
        for p in target.peripherals
        for window in _ARCH_WINDOWS
        if p.base < window + 0x1000 and window < p.base + p.size
    ]
    assert overlapping == []


def test_otp_is_a_read_only_region_with_a_secure_alias(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    regions = {r.name: r for r in target.regions}
    otp, alias = regions["OTP_NS"], regions["OTP_S"]
    assert (otp.start, otp.size, otp.secure, otp.physical) == (0x0BFA_0000, 0x400, False, None)
    assert (alias.start, alias.size, alias.secure, alias.physical) == (
        0x0FFA_0000,
        0x400,
        True,
        0x0BFA_0000,
    )
    for region in (otp, alias):
        assert region.kind is RegionKind.ROM
        assert region.acc == RegionAccess(read=True, write=False, execute=False)


def test_flash_windows_are_served_by_the_flash_interface(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, report = u575_target_and_report
    assert report.controllers_set == 2
    controllers = {r.name: r.controller for r in target.regions}
    assert controllers["Flash_NS"] == "FLASH"
    assert controllers["Flash_S"] == "FLASH"
    assert controllers["OTP_NS"] is None
    assert controllers["OTP_S"] is None
    assert sum(c is not None for c in controllers.values()) == 2
    assert target_peripheral(target, "FLASH").alias_of is None


def test_rng_has_the_nscr_register_st_svd_omits(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    for name in ("RNG", "SEC_RNG"):
        registers = target_peripheral(target, name).registers
        assert [(r.name, r.offset) for r in registers] == [
            ("CR", 0x0),
            ("SR", 0x4),
            ("DR", 0x8),
            ("NSCR", 0xC),
            ("HTCR", 0x10),
        ]
        nscr = registers[3]
        assert (nscr.width, nscr.reset, nscr.acc) == (32, 0x0003_FFFF, Access.READ_WRITE)
        assert [(f.name, f.lsb, f.width) for f in nscr.fields] == [
            (f"EN_OSC{n}", 3 * (n - 1), 3) for n in range(6, 0, -1)
        ]


def test_otg_fs_has_the_core_id_and_hardware_configuration_words(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    for name in ("OTG_FS", "SEC_OTG_FS"):
        registers = {r.name: r for r in target_peripheral(target, name).registers}
        added = [registers[n] for n in ("GSNPSID", "GHWCFG1", "GHWCFG2", "GHWCFG3", "GHWCFG4")]
        assert [(r.offset, r.width, r.reset, r.acc, r.fields) for r in added] == [
            (0x40, 32, 0x4F54_310A, Access.READ_ONLY, []),
            (0x44, 32, 0, Access.READ_ONLY, []),
            (0x48, 32, 0x229E_D520, Access.READ_ONLY, []),
            (0x4C, 32, 0x0200_D1E8, Access.READ_ONLY, []),
            (0x50, 32, 0x17F0_8030, Access.READ_ONLY, []),
        ]
        assert registers["CID"].offset == 0x3C
        assert registers["GLPMCFG"].offset == 0x54


def test_otg_fs_ghwcfg2_describes_six_slave_mode_full_speed_device_endpoints(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    ghwcfg2 = next(r for r in target_peripheral(target, "OTG_FS").registers if r.name == "GHWCFG2")
    assert (ghwcfg2.reset >> 10) & 0xF == 5
    assert (ghwcfg2.reset >> 3) & 0x3 == 0
    assert (ghwcfg2.reset >> 8) & 0x3 == 1
    assert (ghwcfg2.reset >> 6) & 0x3 == 0


def test_otg_fs_has_the_fifo_windows_and_gdfifocfg(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    otg = target_peripheral(target, "OTG_FS")
    sec = target_peripheral(target, "SEC_OTG_FS")
    assert otg.size == 0x20000
    assert sec.size == 0x20000
    assert sec.base == otg.base + 0x1000_0000
    for peripheral in (otg, sec):
        registers = {r.name: r for r in peripheral.registers}
        for n in range(6):
            fifo = registers[f"DFIFO{n}"]
            assert (fifo.offset, fifo.width, fifo.reset, fifo.acc, fifo.fields) == (
                0x1000 * (n + 1),
                32,
                0,
                Access.READ_WRITE,
                [],
            )
            assert fifo.offset + 4 <= peripheral.size
        assert registers["DFIFO3"].offset == 0x4000
        gdfifocfg = registers["GDFIFOCFG"]
        assert (gdfifocfg.offset, gdfifocfg.width, gdfifocfg.reset, gdfifocfg.acc) == (
            0x5C,
            32,
            0,
            Access.READ_WRITE,
        )


def test_the_device_signature_page_holds_package_uid_and_flash_size(
    u575_target_and_report: _TargetAndReport,
) -> None:
    target, _ = u575_target_and_report
    regions = {r.name: r for r in target.regions}
    page, alias = regions["SIG_NS"], regions["SIG_S"]
    assert (page.start, page.size, page.kind, page.physical) == (
        0x0BFA_0400,
        0x400,
        RegionKind.ROM,
        None,
    )
    assert (alias.start, alias.secure, alias.physical, alias.content) == (
        0x0FFA_0400,
        True,
        0x0BFA_0400,
        None,
    )
    assert page.acc == RegionAccess(read=True, write=False, execute=False)
    assert [(piece.offset, bytes(piece.data)) for piece in page.content or []] == [
        (0x100, bytes([0b00011, 0])),
        (0x300, bytes.fromhex("140032") + b"\x00\x01NROCPOP"),
        (0x310, (4107).to_bytes(2, "little")),
        (0x342, (5472).to_bytes(2, "little")),
        (0x3A0, bytes([0x00, 0x04])),
        (0x3A5, (6635).to_bytes(2, "little")),
    ]
