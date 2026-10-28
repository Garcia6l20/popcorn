import json
import tomllib
from dataclasses import replace
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.stm32.description import _patches
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description, build_description_with_report
from popcorn.stm32.description._cubemx import read_part
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import PartDescription, PeripheralDesc
from popcorn.stm32.description._sources import Sources

pytestmark = pytest.mark.timeout(30)


NO_CUBEMX_PERIPHERAL = {
    "CORTEX_M33_NS",
    "CORTEX_M33_S",
    "DEBUG",
    "FILEX",
    "GUI_INTERFACE",
    "HDMI_CEC",
    "I2S1",
    "I2S2",
    "I2S3",
    "LEVELX",
    "LINKEDLIST",
    "NETXDUO",
    "SAU",
    "SYS",
    "SYS_S",
    "THREADX",
    "TRACER_EMB",
    "USBPD",
    "USBX",
    "BOOTPATH",
    "MEMORYMAP",
    "NVIC1",
    "NVIC2",
    "GTZC_S",
}
"""CubeMX instances of the part that no SVD peripheral carries: the core, the middleware, the I2S
views of SPI1 to SPI3, GTZC2 which the part lacks, the HDMI-CEC block the SVD lacks and the DEBUG
instance of DBGMCU."""

NO_IP_VERSION = {"ADCC", "CRS", "DBGMCU", "DLYBOS1", "DLYBSD1", "DLYBSD2", "EXTI"}
"""Peripherals with no CubeMX instance under any name, so their IP version stays empty."""

NAMES_ONLY_IN_CMSIS = {
    111: ("Cordic_IRQn", "CORDIC_IRQn"),
    113: ("DTS_WKUP_IRQn", "DTS_IRQn"),
}
"""IRQ numbers whose name differs between the SVD and the CMSIS header, as (SVD, CMSIS)."""


def _by_name(description: PartDescription) -> dict[str, PeripheralDesc]:
    return {p.name: p for p in description.peripherals}


def _irq_names(description: PartDescription) -> dict[int, str]:
    return {irq.number: irq.name for irq in description.irqs}


def _region_bytes(description: PartDescription, region: str, offset: int, size: int) -> bytes:
    target = next(r for r in description.regions if r.name == region)
    image = bytearray(b"\xff" * target.size)
    for piece in target.content or []:
        image[piece.offset : piece.offset + len(piece.data)] = bytes(piece.data)
    return bytes(image[offset : offset + size])


def test_the_core_comes_from_the_rzone_and_the_patch(h573_sources: Sources) -> None:
    core = build_description(h573_sources).core
    assert core.reset_clock_hz == 32_000_000
    assert core.trustzone is True
    assert core.fpu is True
    assert core.fpu_double is False
    assert core.dsp is True
    assert (core.mpu_regions, core.mpu_regions_secure, core.sau_regions) == (8, 12, 8)
    assert core.num_irqs == 131


def test_peripherals_carry_their_cubemx_ip_and_version(h573_sources: Sources) -> None:
    peripherals = _by_name(build_description(h573_sources))
    expected = ("RCC", "PWR", "FLASH", "UART9", "LPUART", "SPI4", "SPI5", "GPDMA1", "GPDMA2", "USB")
    for name in (*expected, "ICACHE", "DCACHE", "CRC", "SAI1", *(f"GPIO{p}" for p in "ABCDEFGHI")):
        assert name in peripherals, name
    timers = {name for name in peripherals if name.startswith("TIM") and name[3:].isdigit()}
    assert timers == {f"TIM{n}" for n in (*range(1, 9), *range(12, 18))}
    assert peripherals["RCC"].ip_version == "STM32H5_rcc_v1_0"
    assert peripherals["USB"].ip == "USB"
    assert peripherals["LPUART"].ip == "LPUART"
    assert peripherals["LPUART"].ip_version == "sci3_v1_3_Cube"
    assert peripherals["DCACHE"].ip == "DCACHE"
    assert peripherals["DCACHE"].ip_version == "STM32U5xx1_v0_0"
    for port in "ABCDEFGHI":
        assert peripherals[f"GPIO{port}"].ip == "GPIO", port
        assert peripherals[f"GPIO{port}"].ip_version == "STM32H56x_gpio_v1_0", port


def test_only_peripherals_without_a_cubemx_instance_have_no_ip_version(
    h573_sources: Sources,
) -> None:
    description = build_description(h573_sources)
    empty = {p.name for p in description.peripherals if p.alias_of is None and not p.ip_version}
    assert empty == NO_IP_VERSION
    part = read_part(h573_sources.mcu_db.read_bytes(), file=str(h573_sources.mcu_db))
    renamed = {
        instance
        for path in h573_sources.patches
        if path.name in ("stm32h5.toml", "stm32h56x.toml")
        for instance in tomllib.loads(path.read_text(encoding="utf-8"))["ip"]["renames"]
    }
    peripherals = _by_name(description)
    unmatched = {ip.instance for ip in part.ips if ip.instance not in peripherals} - renamed
    assert unmatched == NO_CUBEMX_PERIPHERAL
    for ip in part.ips:
        if ip.instance in renamed:
            assert any(p.ip == ip.name and p.ip_version == ip.version for p in peripherals.values())


def test_the_secure_window_of_lpuart_is_renamed_and_every_alias_derives(
    h573_sources: Sources,
) -> None:
    peripherals = _by_name(build_description(h573_sources))
    assert "SEC_LPUART1" not in peripherals
    assert peripherals["SEC_LPUART"].alias_of == "LPUART"
    assert peripherals["SEC_LPUART"].base == 0x54002400
    assert peripherals["SEC_RCC"].alias_of == "RCC"
    assert peripherals["SEC_RCC"].base == 0x54020C00


def test_interrupts_the_svd_lacks_are_added(h573_sources: Sources) -> None:
    description = build_description(h573_sources)
    names = _irq_names(description)
    assert names[1] == "PVD_AVD_IRQn"
    assert names[2] == "RTC_IRQn"
    assert names[3] == "RTC_S_IRQn"
    assert names[63] == "LPUART1_IRQn"
    assert names[103] == "FPU_IRQn"
    assert names[119] == "CEC_IRQn"
    peripherals = _by_name(description)
    assert peripherals["LPUART"].irqs == [63]
    assert {2, 3} <= set(peripherals["RTC"].irqs)


def test_the_dcache_line_has_one_name_with_and_without_the_cmsis_header(
    h573_sources: Sources,
) -> None:
    for sources in (h573_sources, replace(h573_sources, cmsis_header=None)):
        description = build_description(sources)
        assert _irq_names(description)[105] == "DCACHE1_IRQn"
        assert _by_name(description)["DCACHE"].irqs == [105]


def test_the_cmsis_header_renames_exactly_the_documented_interrupts(
    h573_sources: Sources,
) -> None:
    with_header = build_description(h573_sources)
    without = build_description(replace(h573_sources, cmsis_header=None))
    with_names = _irq_names(with_header)
    without_names = _irq_names(without)
    differing = {
        n: (without_names[n], with_names[n])
        for n in with_names
        if with_names[n] != without_names[n]
    }
    assert differing == NAMES_ONLY_IN_CMSIS

    def content(description: PartDescription) -> dict[str, object]:
        data = cast(dict[str, object], json.loads(dump_json(description)))
        del data["generator"], data["sources"], data["irqs"]
        for peripheral in cast(list[dict[str, object]], data["peripherals"]):
            peripheral.pop("irqs", None)
        return data

    assert content(with_header) == content(without)


def test_the_usb_window_covers_its_packet_memory(h573_sources: Sources) -> None:
    assert _by_name(build_description(h573_sources))["USB"].size == 0xC00


def test_register_names_resolve_bare_and_keep_the_svd_name_as_alias(
    h573_sources: Sources,
) -> None:
    peripherals = _by_name(build_description(h573_sources))
    moder = next(r for r in peripherals["GPIOA"].registers if r.name == "MODER")
    assert moder.aliases == ["GPIOA_MODER"]
    for name in ("UART9", "LPUART"):
        assert "CR1_enabled" in {r.name for r in peripherals[name].registers}


def test_the_regions_and_the_device_signature(h573_sources: Sources) -> None:
    description = build_description(h573_sources)
    regions = {r.name: r for r in description.regions}
    assert (regions["Flash_NS"].start, regions["Flash_NS"].size) == (0x08000000, 0x200000)
    assert regions["Flash_NS"].controller == "FLASH"
    assert regions["Flash_S"].controller == "FLASH"
    assert (regions["SRAM_NS"].start, regions["SRAM_NS"].size) == (0x20000000, 0xA0000)
    assert (regions["OTP_NS"].start, regions["OTP_NS"].size) == (0x08FFF000, 0x800)
    assert regions["OTP_NS"].acc.write is False
    assert (regions["RO_NS"].start, regions["RO_NS"].size) == (0x08FFF800, 0x800)
    assert regions["HIGH_CYCLE_FLASH_NS"].controller is None
    assert _region_bytes(description, "RO_NS", 0x00C, 2) == (2048).to_bytes(2, "little")
    assert _region_bytes(description, "RO_NS", 0x00E, 2) == (0b00110).to_bytes(2, "little")
    assert _region_bytes(description, "RO_NS", 0x010, 2) == (0x05E5).to_bytes(2, "little")
    assert _region_bytes(description, "RO_NS", 0x014, 2) == (0x0301).to_bytes(2, "little")
    assert _region_bytes(description, "RO_NS", 0x018, 2) == (0x03FA).to_bytes(2, "little")


def test_a_flash_clear_write_clears_its_status_flag(h573_sources: Sources) -> None:
    flash = _by_name(build_description(h573_sources))["FLASH"]
    clears = {
        (rule.reg, rule.field, rule.target_reg, rule.target_field)
        for rule in flash.cross_rules
        if rule.reg == "NSCCR"
    }
    assert ("NSCCR", "CLR_EOP", "NSSR", "EOP") in clears
    assert len(clears) == 8


def test_the_digest_registers_reach_shake128(h573_sources: Sources) -> None:
    hash_unit = _by_name(build_description(h573_sources))["HASH"]
    registers = {r.name: r for r in hash_unit.registers}
    assert all(f"HR{n}" in registers for n in range(42))
    assert "HR42" not in registers
    assert registers["HR41"].offset == 0x3B4
    status = {f.name: f for f in registers["SR"].fields}
    assert (status["NBWE"].lsb, status["NBWE"].width) == (16, 6)
    assert (status["NBWP"].lsb, status["NBWP"].width) == (9, 6)


def test_the_rng_follows_rm0481(h573_sources: Sources) -> None:
    rng = _by_name(build_description(h573_sources))["RNG"]
    registers = {r.name: r for r in rng.registers}
    assert registers["CR"].reset == 0x00800D00
    assert registers["HTCR"].reset == 0x000072AC
    status = {f.name: f for f in registers["SR"].fields}
    assert status["SEIS"].on_write.value == "zero_to_clear"
    assert status["CEIS"].on_write.value == "zero_to_clear"


def test_the_exti_pending_flags_and_uart_wakeup_clear_follow_rm0481(h573_sources: Sources) -> None:
    peripherals = _by_name(build_description(h573_sources))
    exti = {r.name: r for r in peripherals["EXTI"].registers}
    for name in ("RPR2", "FPR2"):
        assert {f.on_write.value for f in exti[name].fields} == {"one_to_clear"}
    assert {f.on_write.value for f in exti["RPR1"].fields} == {"one_to_clear"}
    for uart in ("USART1", "UART4", "LPUART"):
        pairs = {(rule.field, rule.target_field) for rule in peripherals[uart].cross_rules}
        assert ("WUCF", "WUF") in pairs
    for uart in ("USART1", "UART4"):
        assert ("UDRCF", "UDR") in {
            (r.field, r.target_field) for r in peripherals[uart].cross_rules
        }


def test_the_rcc_cr_resets_with_hsidiv_one(h573_sources: Sources) -> None:
    rcc = {r.name: r for r in _by_name(build_description(h573_sources))["RCC"].registers}
    assert rcc["CR"].reset == 0x2B


GPIO_RESETS = {
    "MODER": {"A": 0xABFFFFFF, "B": 0xFFFFFEBF, "I": 0x00FFFFFF},
    "OSPEEDR": {"A": 0x0C000000, "B": 0x000000C0},
    "PUPDR": {"A": 0x64000000, "B": 0x00000100},
    "SECCFGR": {"I": 0x00000FFF},
}
GPIO_RESET_DEFAULTS = {"MODER": 0xFFFFFFFF, "OSPEEDR": 0, "PUPDR": 0, "SECCFGR": 0xFFFF}


@pytest.mark.parametrize("port", "ABCDEFGHI")
def test_each_gpio_port_resets_with_its_own_values(h573_sources: Sources, port: str) -> None:
    peripherals = _by_name(build_description(h573_sources))
    for name in (f"GPIO{port}", f"SEC_GPIO{port}"):
        registers = {r.name: r.reset for r in peripherals[name].registers}
        for register, default in GPIO_RESET_DEFAULTS.items():
            assert registers[register] == GPIO_RESETS.get(register, {}).get(
                port, default
            ), f"{name} {register}"


def test_the_dbgmcu_idcode_carries_the_h573_device_id(h573_sources: Sources) -> None:
    dbgmcu = {r.name: r for r in _by_name(build_description(h573_sources))["DBGMCU"].registers}
    assert dbgmcu["IDCODE"].reset & 0xFFF == 0x484
    assert dbgmcu["IDCODE"].reset == 0x10016484


FLASH_CUR_DELIVERY = {
    "OPTSR_CUR": 0x2D30EDF8,
    "OPTSR2_CUR": 0xC300017C,
    "NSBOOTR_CUR": 0x080000C3,
    "SECBOOTR_CUR": 0x0C0000C3,
    "SECWM1R_CUR": 0x0000007F,
    "SECWM2R_CUR": 0x0000007F,
    "HDP1R_CUR": 0x00000001,
    "HDP2R_CUR": 0x00000001,
    "EDATA1R_CUR": 0,
    "EDATA2R_CUR": 0,
    "OTPBLR_CUR": 0,
}


def test_each_flash_cur_register_resets_to_its_table_55_delivery_value(
    h573_sources: Sources,
) -> None:
    peripherals = _by_name(build_description(h573_sources))
    for flash in ("FLASH", "SEC_FLASH"):
        registers = {r.name: r.reset for r in peripherals[flash].registers}
        for name, delivery in FLASH_CUR_DELIVERY.items():
            assert registers[name] == delivery, f"{flash} {name}"


def test_every_patched_reset_is_a_known_value_in_every_bit(h573_sources: Sources) -> None:
    peripherals = _by_name(build_description(h573_sources))
    patch = next(path for path in h573_sources.patches if path.name == "stm32h56x.toml")
    rules = tomllib.loads(patch.read_text(encoding="utf-8"))["resets"]
    assert rules
    for rule in rules:
        for unit in (rule["peripheral"], "SEC_" + rule["peripheral"]):
            if unit not in peripherals:
                continue
            register = next(
                r
                for r in peripherals[unit].registers
                if rule["register"] in (r.name, *(r.aliases or []))
                or rule["register"].removeprefix(rule["peripheral"] + "_") == r.name
            )
            assert register.reset == rule["reset"], f"{unit} {register.name}"
            assert register.reset_mask == (1 << register.width) - 1, f"{unit} {register.name}"


def test_every_rule_of_the_patch_file_applies(h573_sources: Sources) -> None:
    _, report = build_description_with_report(h573_sources)
    assert report == _patches.PatchReport(
        peripherals_renamed=1,
        ip_renamed=19,
        aliased=104,
        fields_touched=408,
        fields_dropped=48,
        fields_renamed=21,
        fields_added=2,
        cross_rules_added=863,
        irqs_dropped=22,
        irqs_added=27,
        sizes_set=1,
        registers_added=27,
        resets_set=37,
        core_fields_set=1,
        regions_added=2,
        controllers_set=2,
        signature_words=6,
    )
    patch = next(path for path in h573_sources.patches if path.name == "stm32h5.toml")
    rules = tomllib.loads(patch.read_text(encoding="utf-8"))
    family = tomllib.loads(
        next(path for path in h573_sources.patches if path.name == "stm32h56x.toml").read_text(
            encoding="utf-8"
        )
    )
    hash_rules = tomllib.loads(
        next(path for path in h573_sources.patches if path.name == "stm32h5hash.toml").read_text(
            encoding="utf-8"
        )
    )
    assert "irqs" not in rules
    assert len(family["irqs"]["add"]) == report.irqs_added
    assert len(family["irqs"]["drop"]) == report.irqs_dropped
    assert "registers" not in rules
    assert len(hash_rules["registers"]) + len(family["registers"]) == report.registers_added
    assert len(rules["memory"]["add"]) == report.regions_added
    assert "drop" not in family["peripherals"]
    assert len(family["peripherals"]["renames"]) == report.peripherals_renamed


def test_the_patch_file_applies_only_to_its_parts(h573_sources: Sources, tmp_path: Path) -> None:
    patch = next(path for path in h573_sources.patches if path.name == "stm32h56x.toml")
    text = patch.read_text(encoding="utf-8").replace('"STM32H573*"', '"STM32H673*"')
    other = tmp_path / patch.name
    other.write_text(text, encoding="utf-8")
    patches = [other if path == patch else path for path in h573_sources.patches]
    with pytest.raises(DescriptionError, match=r"aliases\.SEC_LPUART1"):
        build_description(replace(h573_sources, patches=patches))


def test_a_machine_builds_on_the_description_with_and_without_the_cmsis_header(
    h573_sources: Sources,
) -> None:
    for sources in (h573_sources, replace(h573_sources, cmsis_header=None)):
        description = popcorn.parse_description(dump_json(build_description(sources)), "h573")
        assert popcorn.Machine(description).mem.read(0x08FFF80C, 2) == (2048).to_bytes(2, "little")


def test_an_alternates_rule_drops_the_entry_from_the_built_port(
    h573_sources: Sources, tmp_path: Path
) -> None:
    extra = tmp_path / "drop.toml"
    extra.write_text(
        'format = 1\n[match]\nparts = ["STM32H5*"]\n[alternates]\n'
        'drop = [{ peripheral = "GPIOI", pin = 11, af = 13, signal = "pssi_d15", '
        'source = "probe" }]\n',
        encoding="utf-8",
    )
    plain, _ = build_description_with_report(h573_sources)
    dropped, report = build_description_with_report(
        replace(h573_sources, patches=[*h573_sources.patches, extra])
    )
    assert report.alternates_dropped == 1

    def port(description: PartDescription, name: str) -> list[tuple[int, int, str]]:
        found = next(p for p in description.peripherals if p.name == name)
        return [(a.pin, a.af, a.signal) for a in found.alternates or []]

    assert (11, 13, "pssi_d15") in port(plain, "GPIOI")
    assert port(dropped, "GPIOI") == [a for a in port(plain, "GPIOI") if a[0] != 11]
    assert port(dropped, "GPIOA") == port(plain, "GPIOA")


def test_gpioi_has_afrh_with_afsel8_to_afsel11_and_keeps_pi11_af13(
    h573_sources: Sources,
) -> None:
    description, _ = build_description_with_report(h573_sources)
    port = next(p for p in description.peripherals if p.name == "GPIOI")
    afrh = next(r for r in port.registers if r.name == "AFRH")
    assert afrh.offset == 0x24
    assert sorted((f.name, f.lsb, f.width) for f in afrh.fields) == sorted(
        [
            ("AFSEL8", 0, 4),
            ("AFSEL9", 4, 4),
            ("AFSEL10", 8, 4),
            ("AFSEL11", 12, 4),
        ]
    )
    assert "GPIOI_AFRH" in (afrh.aliases or [])
    assert any((a.pin, a.af, a.signal) == (11, 13, "pssi_d15") for a in port.alternates or [])


def test_gpioi_carries_the_per_pin_fields_of_pins_0_to_11_only(h573_sources: Sources) -> None:
    peripherals = _by_name(build_description(h573_sources))
    prefixes = {
        "MODER": ["MODE"],
        "OTYPER": ["OT"],
        "OSPEEDR": ["OSPEED"],
        "PUPDR": ["PUPD"],
        "IDR": ["ID"],
        "ODR": ["OD"],
        "BSRR": ["BS", "BR"],
        "LCKR": ["LCK"],
        "BRR": ["BR"],
        "HSLVR": ["HSLV"],
        "SECCFGR": ["SEC"],
    }
    for name in ("GPIOI", "SEC_GPIOI", "GPIOH"):
        pins = 12 if name.endswith("GPIOI") else 16
        registers = {r.name: r for r in peripherals[name].registers}
        for register, starts in prefixes.items():
            expected = {f"{start}{pin}" for start in starts for pin in range(pins)}
            if register == "LCKR":
                expected.add("LCKK")
            assert {f.name for f in registers[register].fields} == expected, f"{name} {register}"


def test_tim15_smcr_holds_the_preload_fields_the_svd_lacks(h573_sources: Sources) -> None:
    peripherals = _by_name(build_description(h573_sources))
    for unit in ("TIM15", "SEC_TIM15"):
        smcr = next(r for r in peripherals[unit].registers if r.name in ("SMCR", "TIM15_SMCR"))
        fields = {f.name: (f.lsb, f.width) for f in smcr.fields}
        assert fields["SMSPE"] == (24, 1), unit
        assert fields["SMSPS"] == (25, 1), unit
