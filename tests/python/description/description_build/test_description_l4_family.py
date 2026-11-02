"""The STM32L4 DIE415 rzone facts files and the family patch `stm32l4.toml`, over the pin data."""

import tomllib
from functools import cache
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32.description import _cubemx
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cubemx import expand_refname, read_rzone_facts
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._model import (
    PartDescription,
    PeripheralDesc,
    RegionDesc,
    RegisterDesc,
)
from popcorn.stm32.description._sources import facts_path, find_sources, read_db_part

from ..conftest import require_pin_data

FACTS_DIR = Path(_cubemx.__file__).parent / "rzone"
STEMS = [f"STM32_DIE415_128_{flash}" for flash in (256, 512, 1024)]
LINE_PARTS = ["STM32L471ZGTx", "STM32L475VGTx", "STM32L476RCTx", "STM32L486RGTx"]
EXTI_FED = [2, 3, 41, 64, 81]
UID = (0x0032_0014, 0x4F52_4E01, 0x504F_5043)
PACKAGES = {
    "STM32L476RCTx": 0x00,
    "STM32L476VGTx": 0x02,
    "STM32L476QGIx": 0x03,
    "STM32L476ZGTx": 0x04,
    "STM32L476ZGJx": 0x04,
    "STM32L476JGYx": 0x04,
    "STM32L476MGYx": 0x04,
    "STM32L476VGYxP": 0x04,
}
"""One part per DIE415 package, with its RM0351 49.3 PKG code."""


def _flash_kib(stem: str) -> int:
    return int(stem.rsplit("_", 1)[1])


def test_the_l4_facts_files_are_one_per_flash_size_of_the_die() -> None:
    present = sorted(path.stem for path in FACTS_DIR.glob("STM32_DIE415_*.toml"))
    assert present == sorted(STEMS)


@pytest.mark.parametrize("stem", STEMS)
def test_a_facts_file_reads_and_carries_the_core_of_a_cortex_m4f(stem: str) -> None:
    path = FACTS_DIR / f"{stem}.toml"
    assert tomllib.loads(path.read_text(encoding="utf-8"))["rzone"] == f"{stem}.xml"
    processor = read_rzone_facts(path.read_bytes(), file=str(path)).processor
    assert processor.fpu != "DP_FPU"
    assert (processor.dsp, processor.tz) == (True, False)
    assert (processor.mpu_regions, processor.sec_mpu_regions, processor.sau_regions) == (8, 0, 0)
    assert processor.num_interrupts == 82


@pytest.mark.parametrize("stem", STEMS)
def test_a_facts_file_splits_sram1_and_sram2_and_maps_the_flash_of_its_name(stem: str) -> None:
    path = FACTS_DIR / f"{stem}.toml"
    regions = read_rzone_facts(path.read_bytes(), file=str(path)).regions
    assert [(r.name, r.start, r.size) for r in regions] == [
        ("Flash", 0x0800_0000, _flash_kib(stem) * 1024),
        ("SRAM2", 0x1000_0000, 32 * 1024),
        ("SRAM1", 0x2000_0000, 96 * 1024),
    ]
    flash, sram2, sram1 = regions
    assert flash.acc.execute and not flash.acc.write
    assert all(r.acc.write and r.acc.execute for r in (sram1, sram2))


def test_every_die415_name_of_the_db_selects_one_of_the_facts_files() -> None:
    pin_data = require_pin_data()
    seen: set[str] = set()
    count = 0
    for path in sorted((pin_data / "mcu").glob("STM32L4[78]*.xml")):
        for name, _ in expand_refname(path.stem):
            part = read_db_part(path, name)
            if part.die != "DIE415":
                continue
            stem = facts_path(part).stem
            assert (FACTS_DIR / f"{stem}.toml").is_file(), name
            seen.add(stem)
            count += 1
    assert count == 42
    assert seen == set(STEMS)


@cache
def _description(name: str) -> PartDescription:
    require_pin_data()
    try:
        sources = find_sources(name)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    assert sources.rzone is None and sources.rzone_facts is not None
    return build_description(sources)


def _peripheral(description: PartDescription, name: str) -> PeripheralDesc:
    return next(p for p in description.peripherals if p.name == name)


def _region(description: PartDescription, name: str) -> RegionDesc:
    return next(r for r in description.regions if r.name == name)


def _info_bytes(description: PartDescription, offset: int, size: int) -> bytes:
    info = _region(description, "INFO")
    data = bytearray(b"\xff" * info.size)
    for piece in info.content or []:
        data[piece.offset : piece.offset + len(piece.data)] = bytes(piece.data)
    return bytes(data[offset : offset + size])


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_exti_fed_lines_belong_to_exti_on_each_svd(name: str) -> None:
    description = _description(name)
    assert set(EXTI_FED) <= set(_peripheral(description, "EXTI").irqs)
    for owner in ("RTC", "COMP"):
        assert not set(EXTI_FED) & set(_peripheral(description, owner).irqs), owner
    numbers = {irq.number for irq in description.irqs}
    assert set(EXTI_FED) <= numbers


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_family_patch_sets_the_core_and_the_ip_identities(name: str) -> None:
    description = _description(name)
    assert description.core.reset_clock_hz == 4_000_000
    assert description.core.cpu == "CM4"
    assert (description.core.fpu, description.core.fpu_double) == (True, False)
    assert (description.core.mpu, description.core.mpu_regions) == (True, 8)
    assert not description.core.trustzone
    identities = {
        p.name: (p.ip, p.ip_version)
        for p in description.peripherals
        if p.name in ("GPIOA", "GPIOH", "DMA1", "DMA2", "SYSCFG", "COMP", "OPAMP")
    }
    assert identities == {
        "GPIOA": ("GPIO", "STM32L47x_gpio_v1_0"),
        "GPIOH": ("GPIO", "STM32L47x_gpio_v1_0"),
        "DMA1": ("DMA", "STM32L4A6_dma_v1_1"),
        "DMA2": ("DMA", "STM32L4A6_dma_v1_1"),
        "SYSCFG": ("SYS", "STM32L47x_sys_v1_0"),
        "COMP": ("COMP", "TSMC90_Cube"),
        "OPAMP": ("OPAMP", "tsmc018_ull_opamp_v1_0_L4_Cube"),
    }


@pytest.mark.timeout(60)
def test_dbgmcu_idcode_resets_to_dev_id_0x415_rev_4() -> None:
    dbgmcu = _peripheral(_description("STM32L476RCTx"), "DBGMCU")
    idcode = next(r for r in dbgmcu.registers if r.name == "IDCODE")
    assert idcode.reset == 0x1007_6415


@pytest.mark.timeout(60)
def test_the_memory_map_carries_the_information_block_and_the_bit_band_windows() -> None:
    description = _description("STM32L476RCTx")
    added = {
        r.name: (r.start, r.size, r.kind.value, r.controller, r.physical)
        for r in description.regions
        if r.name not in ("Flash", "SRAM1", "SRAM2")
    }
    assert added == {
        "SYSTEM": (0x1FFF_0000, 0x7000, "rom", None, None),
        "INFO": (0x1FFF_7000, 0x1000, "rom", "FLASH", None),
        "SYSTEM2": (0x1FFF_8000, 0x7000, "rom", None, None),
        "OPTION2": (0x1FFF_F000, 0x1000, "rom", "FLASH", None),
        "BB_SRAM1": (0x2200_0000, 0x0200_0000, "bit_band", None, 0x2000_0000),
        "BB_PERIPH": (0x4200_0000, 0x0200_0000, "bit_band", None, 0x4000_0000),
    }
    assert _region(description, "Flash").controller == "FLASH"
    assert _region(description, "SYSTEM").acc.execute
    assert not _region(description, "INFO").acc.execute


@pytest.mark.timeout(60)
@pytest.mark.parametrize(("name", "flash_kib"), [("STM32L476RCTx", 256), ("STM32L476RGTx", 1024)])
def test_the_signature_holds_the_uid_and_the_flash_size(name: str, flash_kib: int) -> None:
    description = _description(name)
    uid = _info_bytes(description, 0x590, 12)
    assert tuple(int.from_bytes(uid[i : i + 4], "little") for i in (0, 4, 8)) == UID
    assert int.from_bytes(_info_bytes(description, 0x5E0, 2), "little") == flash_kib


@pytest.mark.timeout(120)
@pytest.mark.parametrize(("name", "code"), sorted(PACKAGES.items()))
def test_the_package_register_holds_the_pkg_code_of_the_package(name: str, code: int) -> None:
    assert int.from_bytes(_info_bytes(_description(name), 0x500, 2), "little") == code


@pytest.mark.timeout(60)
def test_the_calibration_words_are_the_datasheet_typicals() -> None:
    description = _description("STM32L476RCTx")
    words = {
        offset: int.from_bytes(_info_bytes(description, offset, 2), "little")
        for offset in (0x5A8, 0x5AA, 0x5CA)
    }
    assert words == {0x5A8: 1037, 0x5AA: 1654, 0x5CA: 1310}


USARTS = ["USART1", "USART2", "USART3", "UART4", "UART5"]
RESETS: dict[tuple[str, str], int] = {
    ("EXTI", "IMR2"): 0x0000_0087,
    ("GPIOA", "MODER"): 0xABFF_FFFF,
    ("GPIOB", "MODER"): 0xFFFF_FEBF,
    **{(f"GPIO{port}", "MODER"): 0xFFFF_FFFF for port in "CDEFG"},
    ("GPIOH", "MODER"): 0x0000_000F,
    ("GPIOA", "OSPEEDR"): 0x0C00_0000,
    ("GPIOB", "OSPEEDR"): 0,
    ("LPUART1", "ISR"): 0x0000_00C0,
    **{(tim, "ARR"): 0xFFFF for tim in ("TIM1", "TIM6", "TIM7", "TIM8", "TIM15", "TIM16", "TIM17")},
    **{(tim, "ARR"): 0xFFFF_FFFF for tim in ("TIM2", "TIM3", "TIM4", "TIM5")},
    ("TIM16", "OR2"): 1,
    ("TIM17", "OR2"): 1,
    **{(adc, "CR"): 0x2000_0000 for adc in ("ADC1", "ADC2", "ADC3")},
    **{(adc, "CFGR"): 0x8000_0000 for adc in ("ADC1", "ADC2", "ADC3")},
    **{(adc, "TR2"): 0x00FF_0000 for adc in ("ADC1", "ADC2", "ADC3")},
    **{(adc, "TR3"): 0x00FF_0000 for adc in ("ADC1", "ADC2", "ADC3")},
    **{(sai, reg): 8 for sai in ("SAI1", "SAI2") for reg in ("ASR", "BSR")},
    ("DAC1", "SHRR"): 0x0001_0001,
}
"""Every register reset 03a2 corrects, with its RM0351 value."""


def _fields(
    description: PartDescription, peripheral: str, register: str
) -> dict[str, tuple[int, int]]:
    reg = next(r for r in _peripheral(description, peripheral).registers if r.name == register)
    return {f.name: (f.lsb, f.width) for f in reg.fields}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_usart_brr_is_one_field_and_the_lpuart_brr_keeps_twenty_bits(name: str) -> None:
    description = _description(name)
    for usart in USARTS:
        assert _fields(description, usart, "BRR") == {"BRR": (0, 16)}, usart
    assert _fields(description, "LPUART1", "BRR") == {"BRR": (0, 20)}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_usart_control_fields_take_the_rm0351_names(name: str) -> None:
    description = _description(name)
    for usart in [*USARTS, "LPUART1"]:
        cr1 = _fields(description, usart, "CR1")
        cr2 = _fields(description, usart, "CR2")
        cr3 = _fields(description, usart, "CR3")
        assert (cr1["DEAT"], cr1["DEDT"]) == ((21, 5), (16, 5)), usart
        assert not {f"DEAT{i}" for i in range(5)} & set(cr1), usart
        assert (cr2["ADD"], cr2["DATAINV"]) == ((24, 8), (18, 1)), usart
        assert not {"ADD0_3", "ADD4_7", "TAINV"} & set(cr2), usart
        assert cr3["UCESM"] == (23, 1), usart
        assert "UCESM_" not in cr3, usart
        if usart == "LPUART1":
            assert not {"ABRMOD", "CLKEN"} & set(cr2)
        else:
            assert (cr2["ABRMOD"], cr2["CLKEN"]) == ((21, 2), (11, 1)), usart


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_dbgmcu_freeze_bits_take_the_rm0351_names(name: str) -> None:
    fzr1 = _fields(_description(name), "DBGMCU", "APB1_FZR1")
    assert {
        k: fzr1[k] for k in ("DBG_TIM2_STOP", "DBG_TIM6_STOP", "DBG_CAN1_STOP", "DBG_LPTIM1_STOP")
    } == {
        "DBG_TIM2_STOP": (0, 1),
        "DBG_TIM6_STOP": (4, 1),
        "DBG_CAN1_STOP": (25, 1),
        "DBG_LPTIM1_STOP": (31, 1),
    }


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_every_corrected_reset_holds_its_rm0351_value(name: str) -> None:
    description = _description(name)
    got = {
        (peripheral, register): next(
            r for r in _peripheral(description, peripheral).registers if r.name == register
        ).reset
        for peripheral, register in RESETS
    }
    assert got == RESETS


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_advanced_timer_repetition_counter_has_sixteen_bits(name: str) -> None:
    description = _description(name)
    assert _fields(description, "TIM1", "RCR")["REP"] == (0, 16)
    assert _fields(description, "TIM8", "RCR")["REP"] == (0, 16)
    assert _fields(description, "TIM15", "RCR")["REP"] == (0, 8)


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_dac_is_dac1_with_its_cubemx_identity(name: str) -> None:
    description = _description(name)
    assert "DAC" not in {p.name for p in description.peripherals}
    dac = _peripheral(description, "DAC1")
    assert (dac.ip, dac.ip_version) == ("DAC", "dacif_v2_0_Cube")


OTG_PARTS = ["STM32L475VGTx", "STM32L476RCTx", "STM32L486RGTx"]
OTG_SPLIT = ["OTG_FS_GLOBAL", "OTG_FS_HOST", "OTG_FS_DEVICE", "OTG_FS_PWRCLK"]
OTG_OFFSETS = {
    "GOTGCTL": 0x000,
    "GRXSTSR_DEVICE": 0x01C,
    "GRXSTSR_HOST": 0x01C,
    "HNPTXFSIZ": 0x028,
    "HNPTXSTS": 0x02C,
    "GSNPSID": 0x040,
    "GHWCFG1": 0x044,
    "GHWCFG2": 0x048,
    "GHWCFG3": 0x04C,
    "GHWCFG4": 0x050,
    "GDFIFOCFG": 0x05C,
    "HCFG": 0x400,
    "HCTSIZ7": 0x5F0,
    "DCFG": 0x800,
    "DOEPTSIZ3": 0xB70,
    "PCGCCTL": 0xE00,
    **{f"DFIFO{n}": 0x1000 * (n + 1) for n in range(6)},
}
"""Registers of each of the four SVD blocks and of the adds, at their RM0351 47.15.57 offsets."""
OTG_FIELDS: dict[str, dict[str, tuple[int, int]]] = {
    "GOTGCTL": {
        "OTGVER": (20, 1),
        "EHEN": (12, 1),
        "BVALOVAL": (7, 1),
        "BVALOEN": (6, 1),
        "AVALOVAL": (5, 1),
        "AVALOEN": (4, 1),
        "VBVALOVAL": (3, 1),
        "VBVALOEN": (2, 1),
    },
    "GAHBCFG": {"GINTMSK": (0, 1)},
    "GINTSTS": {"LPMINT": (27, 1), "RSTDET": (23, 1), "IPXFR": (21, 1), "GONAKEFF": (7, 1)},
    "GINTMSK": {"LPMINTM": (27, 1), "RSTDETM": (23, 1), "IPXFRM": (21, 1)},
    "GRXSTSR_HOST": {"PKTSTS": (17, 4), "DPID": (15, 2), "BCNT": (4, 11), "CHNUM": (0, 4)},
    "HNPTXFSIZ": {"NPTXFD": (16, 16), "NPTXFSA": (0, 16)},
    "GCCFG": {
        "VBDEN": (21, 1),
        "SDEN": (20, 1),
        "PDEN": (19, 1),
        "DCDEN": (18, 1),
        "BCDEN": (17, 1),
        "PWRDWN": (16, 1),
        "PS2DET": (3, 1),
        "SDET": (2, 1),
        "PDET": (1, 1),
        "DCDET": (0, 1),
    },
    "HFIR": {"RLDCTRL": (16, 1)},
    **{f"HCTSIZ{n}": {"DOPNG": (31, 1)} for n in range(8)},
    "DCTL": {"DSBESLRJCT": (18, 1)},
    "DSTS": {"DEVLNSTS": (22, 2)},
    "DIEPMSK": {"NAKM": (13, 1)},
    "DOEPMSK": {"NAKMSK": (13, 1), "BERRM": (12, 1), "OUTPKTERRM": (8, 1), "STSPHSRXM": (5, 1)},
    "DAINTMSK": {"OEPM": (16, 16)},
    "DVBUSPULSE": {"DVBUSP": (0, 16)},
    **{
        f"DIEPCTL{n}": {
            "SODDFRM": (29, 1),
            "SD0PID_SEVNFRM": (28, 1),
            "STALL": (21, 1),
            "EONUM_DPIP": (16, 1),
        }
        for n in range(1, 4)
    },
    **{f"DIEPINT{n}": {"NAK": (13, 1), "PKTDRPSTS": (11, 1), "INEPNM": (5, 1)} for n in range(4)},
    "DOEPCTL0": {"STALL": (21, 1)},
    **{
        f"DOEPCTL{n}": {
            "SD1PID_SODDFRM": (29, 1),
            "SD0PID_SEVNFRM": (28, 1),
            "STALL": (21, 1),
            "EONUM_DPIP": (16, 1),
        }
        for n in range(1, 4)
    },
    **{f"DOEPINT{n}": {"NAK": (13, 1), "BERR": (12, 1), "STSPHSRX": (5, 1)} for n in range(4)},
    **{f"DOEPTSIZ{n}": {"RXDPID": (29, 2)} for n in range(1, 4)},
    "PCGCCTL": {"SUSP": (7, 1), "PHYSLEEP": (6, 1), "ENL1GTG": (5, 1)},
}
"""The fields 03a3a corrects to RM0351 47.15, spelled as the OTG model reads them."""
OTG_ABSENT: dict[str, set[str]] = {
    "GOTGCTL": {"CURMOD"},
    "GUSBCFG": {"CTXPKT"},
    "GRSTCTL": {"HSRST", "PSRST"},
    "GINTMSK": {"EPMISM"},
    "GRXSTSR_HOST": {"EPNUM", "FRMNUM"},
    "GCCFG": {"VBUSASEN", "VBUSBSEN", "SOFOUTEN"},
    **{f"HCINTMSK{n}": {"NYET"} for n in range(8)},
    **{f"DOEPINT{n}": {"B2BSTUP"} for n in range(4)},
}
"""Fields RM0351 47.15 and Table 322 leave reserved on STM32L47x/L48x."""
OTG_ACCESS: dict[tuple[str, str], str] = {
    ("GUSBCFG", "PHYSEL"): "read_only",
    ("GINTMSK", "PRTIM"): "read_write",
    ("HPTXSTS", "PTXFSAVL"): "read_only",
    **{("DCTL", f): "write_only" for f in ("SGINAK", "CGINAK", "SGONAK", "CGONAK")},
    ("DIEPCTL0", "EPENA"): "read_write",
    ("DIEPCTL0", "EPDIS"): "read_write",
    ("PCGCCTL", "PHYSUSP"): "read_only",
}
OTG_RESETS: dict[str, int] = {
    "GOTGCTL": 0x0001_0000,
    "GUSBCFG": 0x0000_1440,
    "GRSTCTL": 0x8000_0000,
    "HNPTXFSIZ": 0x0200_0200,
    "CID": 0x0000_2000,
    "GSNPSID": 0x4F54_310A,
    "GHWCFG1": 0,
    "GHWCFG2": 0x229E_D520,
    "GHWCFG3": 0x0200_D1E8,
    "GHWCFG4": 0x17F0_8030,
    "GDFIFOCFG": 0,
    "HPTXFSIZ": 0x0200_0400,
    "DIEPTXF1": 0x0200_0400,
    "DIEPTXF2": 0x0200_0600,
    "DIEPTXF3": 0x0200_0800,
    "DCTL": 0x0000_0002,
    **{f"DTXFSTS{n}": 0x0000_0200 for n in range(4)},
    "PCGCCTL": 0x200B_8000,
}
"""Every OTG register reset 03a3a corrects or adds, with its RM0351 47.15 value or the TinyUSB
ST L476 FS core identity."""


def _registers(description: PartDescription, peripheral: str) -> dict[str, RegisterDesc]:
    return {r.name: r for r in _peripheral(description, peripheral).registers}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_the_otg_is_one_block_on_a_1_kb_page_with_its_irq_and_identity(name: str) -> None:
    description = _description(name)
    names = {p.name for p in description.peripherals}
    assert not set(OTG_SPLIT) & names
    otg = _peripheral(description, "OTG_FS")
    assert (otg.base, otg.size, otg.base % 1024) == (0x5000_0000, 0x20000, 0)
    assert otg.irqs == [67]
    assert (otg.ip, otg.ip_version) == ("USB_OTG_FS", "otgfs1_v2_0_Cube")
    offsets = {r.name: r.offset for r in otg.registers}
    assert {k: offsets.get(k) for k in OTG_OFFSETS} == OTG_OFFSETS
    assert [r.name for r in otg.registers if r.offset == 0x028] == ["HNPTXFSIZ"]


@pytest.mark.timeout(60)
def test_the_otg_patch_leaves_the_l471_unmerged() -> None:
    names = {p.name for p in _description("STM32L471ZGTx").peripherals}
    assert "OTG_FS" not in names
    assert set(OTG_SPLIT) <= names


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_the_otg_fields_follow_rm0351_in_the_spelling_the_model_reads(name: str) -> None:
    registers = _registers(_description(name), "OTG_FS")
    got = {
        register: {f.name: (f.lsb, f.width) for f in registers[register].fields if f.name in want}
        for register, want in OTG_FIELDS.items()
    }
    assert got == OTG_FIELDS
    present = {
        register: {f.name for f in registers[register].fields} & absent
        for register, absent in OTG_ABSENT.items()
    }
    assert present == {register: set() for register in OTG_ABSENT}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_the_otg_field_access_follows_rm0351(name: str) -> None:
    registers = _registers(_description(name), "OTG_FS")
    got = {
        (register, field): next(f for f in registers[register].fields if f.name == field).acc.value
        for register, field in OTG_ACCESS
    }
    assert got == OTG_ACCESS


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_every_corrected_otg_reset_holds_its_rm0351_value(name: str) -> None:
    registers = _registers(_description(name), "OTG_FS")
    assert {register: registers[register].reset for register in OTG_RESETS} == OTG_RESETS


@pytest.mark.timeout(120)
def test_an_stm32l476rctx_machine_attaches_and_serves_the_merged_otg() -> None:
    description = _description("STM32L476RCTx")
    machine = popcorn.Machine(popcorn.parse_description(dump_json(description), "STM32L476RCTx"))
    machine.reset()
    otg = machine.periph.OTG_FS
    assert (otg.GSNPSID.value, otg.DCFG.value, otg.PCGCCTL.value) == (
        0x4F54_310A,
        0x0220_0000,
        0x200B_8000,
    )


OTG_MODEL_FIELDS: dict[str, set[str]] = {
    "GOTGCTL": {"BVALOEN", "BVALOVAL", "BSVLD", "SRQ", "HNPRQ"},
    "GOTGINT": {"SEDET"},
    "GAHBCFG": {"GINTMSK", "TXFELVL"},
    "GUSBCFG": {"FHMOD"},
    "GRSTCTL": {"CSRST", "RXFFLSH", "TXFFLSH", "TXFNUM"},
    "GINTSTS": set(
        str.split(
            "USBRST ENUMDNE SOF EOPF ESUSP USBSUSP WKUPINT IISOIXFR IPXFR OTGINT RXFLVL NPTXFE "
            "GINAKEFF GONAKEFF IEPINT OEPINT MMIS ISOODRP RSTDET LPMINT CIDSCHG DISCINT SRQINT"
        )
    ),
    "GINTMSK": set(),
    "GRXSTSR_DEVICE": set(),
    "GRXSTSP_DEVICE": set(),
    "GRXFSIZ": {"RXFD"},
    "HNPTXFSIZ": {"NPTXFD"},
    "GCCFG": {"PWRDWN", "VBDEN", "BCDEN"},
    **{f"DIEPTXF{n}": {"INEPTXFD"} for n in range(1, 6)},
    "DCFG": {"DAD", "PFIVL"},
    "DCTL": {"SDIS", "GINSTS", "GONSTS", "SGINAK", "CGINAK", "SGONAK", "CGONAK", "TCTL"},
    "DSTS": {"SUSPSTS", "ENUMSPD", "FNSOF"},
    "DIEPMSK": set(),
    "DOEPMSK": set(),
    "DAINT": set(),
    "DAINTMSK": set(),
    "DIEPEMPMSK": {"INEPTXFEM"},
    **{
        f"DIEPCTL{n}": {"EPTYP", "MPSIZ", "USBAEP", "NAKSTS", "STALL", "TXFNUM", "CNAK", "SNAK"}
        | ({"SD0PID_SEVNFRM", "SODDFRM", "EONUM_DPIP"} if n else set())
        | {"EPDIS", "EPENA"}
        for n in range(6)
    },
    **{f"DIEPINT{n}": {"XFRC", "EPDISD", "INEPNE", "TXFE"} for n in range(6)},
    **{f"DIEPTSIZ{n}": {"XFRSIZ", "PKTCNT"} for n in range(6)},
    **{f"DTXFSTS{n}": {"INEPTFSAV"} for n in range(6)},
    **{
        f"DOEPCTL{n}": {"EPTYP", "USBAEP", "MPSIZ", "NAKSTS", "STALL", "CNAK", "SNAK"}
        | ({"SD0PID_SEVNFRM", "SD1PID_SODDFRM", "EONUM_DPIP"} if n else set())
        | {"EPDIS", "EPENA"}
        for n in range(6)
    },
    **{f"DOEPINT{n}": {"XFRC", "EPDISD", "STUP"} for n in range(6)},
    **{f"DOEPTSIZ{n}": {"XFRSIZ", "PKTCNT"} | ({"STUPCNT"} if n == 0 else set()) for n in range(6)},
    **{f"DFIFO{n}": set() for n in range(6)},
}
"""Every register and field `otg_fs.cpp` looks up, except PSRST and FSRST, which the L4 OTG
variant of step 17 names or declares absent."""
OTG_ADDS: dict[str, int] = {
    "GRXSTSP_DEVICE": 0x020,
    "GLPMCFG": 0x054,
    "GPWRDN": 0x058,
    "GADPCTL": 0x060,
    "DIEPTXF4": 0x110,
    "DIEPTXF5": 0x114,
    **{
        f"{reg}{x}": 0x500 + delta + 0x20 * x
        for x in range(8, 12)
        for reg, delta in (("HCCHAR", 0), ("HCINT", 8), ("HCINTMSK", 0xC), ("HCTSIZ", 0x10))
    },
    **{
        f"{reg}{x}": 0x900 + delta + 0x20 * x
        for x in (4, 5)
        for reg, delta in (("DIEPCTL", 0), ("DIEPINT", 8), ("DIEPTSIZ", 0x10), ("DTXFSTS", 0x18))
    },
    **{
        f"{reg}{x}": 0xB00 + delta + 0x20 * x
        for x in (4, 5)
        for reg, delta in (("DOEPCTL", 0), ("DOEPINT", 8), ("DOEPTSIZ", 0x10))
    },
}
"""The 36 registers 03a3b adds, at their RM0351 47.15 offsets."""
OTG_ADDED_FIELDS: dict[str, dict[str, tuple[int, int]]] = {
    "GLPMCFG": {
        "ENBESL": (28, 1),
        "LPMRCNTSTS": (25, 3),
        "SNDLPM": (24, 1),
        "LPMRCNT": (21, 3),
        "LPMCHIDX": (17, 4),
        "L1RSMOK": (16, 1),
        "SLPSTS": (15, 1),
        "LPMRSP": (13, 2),
        "L1DSEN": (12, 1),
        "BESLTHRS": (8, 4),
        "L1SSEN": (7, 1),
        "REMWAKE": (6, 1),
        "BESL": (2, 4),
        "LPMACK": (1, 1),
        "LPMEN": (0, 1),
    },
    "GPWRDN": {"ADPIF": (23, 1), "ADPMEN": (0, 1)},
    "GADPCTL": {
        "AR": (27, 2),
        "ADPTOIM": (26, 1),
        "ADPSNSIM": (25, 1),
        "ADPPRBIM": (24, 1),
        "ADPTOIF": (23, 1),
        "ADPSNSIF": (22, 1),
        "ADPPRBIF": (21, 1),
        "ADPEN": (20, 1),
        "ADPRST": (19, 1),
        "ENASNS": (18, 1),
        "ENAPRB": (17, 1),
        "RTIM": (6, 11),
        "PRBPER": (4, 2),
        "PRBDELTA": (2, 2),
        "PRBDSCHG": (0, 2),
    },
}
"""RM0351 47.15.17 to 47.15.19."""
OTG_ADDED_FROM: dict[str, str] = {
    "GRXSTSP_DEVICE": "GRXSTSR_DEVICE",
    "DIEPTXF4": "DIEPTXF3",
    "DIEPTXF5": "DIEPTXF3",
    **{
        f"{reg}{x}": f"{reg}7"
        for x in range(8, 12)
        for reg in ("HCCHAR", "HCINT", "HCINTMSK", "HCTSIZ")
    },
    **{
        f"{reg}{x}": f"{reg}3"
        for x in (4, 5)
        for reg in ("DIEPCTL", "DIEPINT", "DIEPTSIZ", "DTXFSTS")
    },
    **{f"{reg}{x}": f"{reg}3" for x in (4, 5) for reg in ("DOEPCTL", "DOEPINT", "DOEPTSIZ")},
}
"""Each added endpoint, channel and FIFO register and the register whose RM0351 layout it shares."""
OTG_ADDED_RESETS = {"DIEPTXF4": 0x0200_0A00, "DIEPTXF5": 0x0200_0C00, "GPWRDN": 0x0000_0010}
OTG_ADDED_ACCESS = {
    **{("GLPMCFG", f): "read_only" for f in ("LPMRCNTSTS", "L1RSMOK", "SLPSTS", "LPMRSP")},
    ("GLPMCFG", "LPMEN"): "read_write",
    ("GADPCTL", "RTIM"): "read_only",
    ("GADPCTL", "AR"): "read_write",
}


def _layout(register: RegisterDesc) -> list[tuple[str, int, int, str]]:
    return sorted((f.name, f.lsb, f.width, f.acc.value) for f in register.fields)


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_every_register_and_field_the_otg_model_reads_is_present(name: str) -> None:
    registers = _registers(_description(name), "OTG_FS")
    missing = {
        register: wanted - {f.name for f in registers[register].fields}
        for register, wanted in OTG_MODEL_FIELDS.items()
        if register in registers
    }
    assert set(OTG_MODEL_FIELDS) <= set(registers)
    assert missing == {register: set() for register in OTG_MODEL_FIELDS}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("name", OTG_PARTS)
def test_the_added_otg_registers_follow_rm0351(name: str) -> None:
    registers = _registers(_description(name), "OTG_FS")
    assert {r: registers[r].offset for r in OTG_ADDS if r in registers} == OTG_ADDS
    got_fields = {
        r: {f.name: (f.lsb, f.width) for f in registers[r].fields} for r in OTG_ADDED_FIELDS
    }
    assert got_fields == OTG_ADDED_FIELDS
    shared = {
        added: (_layout(registers[added]), registers[added].acc, registers[added].width)
        for added in OTG_ADDED_FROM
    }
    assert shared == {
        added: (_layout(registers[ref]), registers[ref].acc, registers[ref].width)
        for added, ref in OTG_ADDED_FROM.items()
    }
    resets = {r: registers[r].reset for r in OTG_ADDED_FROM if r not in OTG_ADDED_RESETS}
    assert resets == {
        r: registers[OTG_ADDED_FROM[r]].reset for r in OTG_ADDED_FROM if r not in OTG_ADDED_RESETS
    }
    assert {r: registers[r].reset for r in OTG_ADDED_RESETS} == OTG_ADDED_RESETS
    access = {
        (r, f): next(x for x in registers[r].fields if x.name == f).acc.value
        for r, f in OTG_ADDED_ACCESS
    }
    assert access == OTG_ADDED_ACCESS
