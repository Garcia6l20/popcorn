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
    27: ("GPDMA1_CH0_IRQn", "GPDMA1_Channel0_IRQn"),
    28: ("GPDMA1_CH1_IRQn", "GPDMA1_Channel1_IRQn"),
    29: ("GPDMA1_CH2_IRQn", "GPDMA1_Channel2_IRQn"),
    30: ("GPDMA1_CH3_IRQn", "GPDMA1_Channel3_IRQn"),
    31: ("GPDMA1_CH4_IRQn", "GPDMA1_Channel4_IRQn"),
    32: ("GPDMA1_CH5_IRQn", "GPDMA1_Channel5_IRQn"),
    33: ("GPDMA1_CH6_IRQn", "GPDMA1_Channel6_IRQn"),
    34: ("GPDMA1_CH7_IRQn", "GPDMA1_Channel7_IRQn"),
    41: ("TIM1_BRK_TERR_IERR_IRQn", "TIM1_BRK_IRQn"),
    43: ("TIM1_TRG_COM_DIR_IDX_IRQn", "TIM1_TRG_COM_IRQn"),
    65: ("IM8_BRK_TERR_IERR_IRQn", "TIM8_BRK_IRQn"),
    67: ("TIM8_TRG_COM_DIR_IDX_IRQn", "TIM8_TRG_COM_IRQn"),
    74: ("USB_FS_IRQn", "USB_DRD_FS_IRQn"),
    90: ("GPDMA2_CH0_IRQn", "GPDMA2_Channel0_IRQn"),
    91: ("GPDMA2_CH1_IRQn", "GPDMA2_Channel1_IRQn"),
    92: ("GPDMA2_CH2_IRQn", "GPDMA2_Channel2_IRQn"),
    93: ("GPDMA2_CH3_IRQn", "GPDMA2_Channel3_IRQn"),
    94: ("GPDMA2_CH4_IRQn", "GPDMA2_Channel4_IRQn"),
    95: ("GPDMA2_CH5_IRQn", "GPDMA2_Channel5_IRQn"),
    96: ("GPDMA2_CH6_IRQn", "GPDMA2_Channel6_IRQn"),
    97: ("GPDMA2_CH7_IRQn", "GPDMA2_Channel7_IRQn"),
    105: ("DCACHE_IRQn", "DCACHE1_IRQn"),
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
    patch = next(path for path in h573_sources.patches if path.name == "stm32h5.toml")
    renamed = set(tomllib.loads(patch.read_text(encoding="utf-8"))["ip"]["renames"])
    peripherals = _by_name(description)
    unmatched = {ip.instance for ip in part.ips if ip.instance not in peripherals} - renamed
    assert unmatched == NO_CUBEMX_PERIPHERAL
    for ip in part.ips:
        if ip.instance in renamed:
            assert any(p.ip == ip.name and p.ip_version == ip.version for p in peripherals.values())


def test_the_secure_alias_of_lpuart1_is_dropped_and_the_others_derive(
    h573_sources: Sources,
) -> None:
    peripherals = _by_name(build_description(h573_sources))
    assert "SEC_LPUART1" not in peripherals
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


@pytest.mark.timeout(30)
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


def test_every_rule_of_the_patch_file_applies(h573_sources: Sources) -> None:
    _, report = build_description_with_report(h573_sources)
    assert report == _patches.PatchReport(
        peripherals_dropped=1,
        ip_renamed=19,
        aliased=103,
        fields_touched=60,
        cross_rules_added=847,
        irqs_dropped=0,
        irqs_added=5,
        sizes_set=1,
        registers_added=26,
        resets_set=28,
        core_fields_set=1,
        regions_added=2,
        controllers_set=2,
        signature_words=6,
    )
    patch = next(path for path in h573_sources.patches if path.name == "stm32h5.toml")
    rules = tomllib.loads(patch.read_text(encoding="utf-8"))
    assert len(rules["irqs"]["add"]) == report.irqs_added
    assert len(rules["registers"]) == report.registers_added
    assert len(rules["memory"]["add"]) == report.regions_added
    assert len(rules["peripherals"]["drop"]) == report.peripherals_dropped


def test_the_patch_file_applies_only_to_its_parts(h573_sources: Sources, tmp_path: Path) -> None:
    patch = next(path for path in h573_sources.patches if path.name == "stm32h5.toml")
    text = patch.read_text(encoding="utf-8").replace('parts = ["STM32H5*"]', 'parts = ["STM32H6*"]')
    other = tmp_path / patch.name
    other.write_text(text, encoding="utf-8")
    patches = [other if path == patch else path for path in h573_sources.patches]
    with pytest.raises(DescriptionError, match=r"PVD_AVD_IRQn\(1\), RTC_IRQn\(2\)"):
        build_description(replace(h573_sources, patches=patches))


def test_a_machine_builds_on_the_description_with_and_without_the_cmsis_header(
    h573_sources: Sources,
) -> None:
    for sources in (h573_sources, replace(h573_sources, cmsis_header=None)):
        description = popcorn.parse_description(dump_json(build_description(sources)), "h573")
        assert popcorn.Machine(description).mem.read(0x08FFF80C, 2) == (2048).to_bytes(2, "little")
