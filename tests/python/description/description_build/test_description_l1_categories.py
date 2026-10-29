"""The STM32L1 category patches over the pin data: each of the 132 names against its category."""

import hashlib
import os
import re
from collections import Counter
from functools import cache
from pathlib import Path

import pytest
from popcorn.stm32.description import _cubemx
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cubemx import DbPart, expand_refname, read_part
from popcorn.stm32.description._irqs import read_cmsis_irqs
from popcorn.stm32.description._model import PartDescription, PeripheralDesc, RegionDesc
from popcorn.stm32.description._patches import cmsis_suffix, load_patches
from popcorn.stm32.description._sources import find_sources

from ..conftest import PIN_DATA_VARIABLE, require_pin_data

PATCH_DIR = Path(_cubemx.__file__).parent / "patches"
INDIVIDUAL = [
    "stm32l1_cat1.toml",
    "stm32l1_cat2.toml",
    "stm32l1_cat3_die427.toml",
    "stm32l1_cat3_die436.toml",
    "stm32l1_cat4.toml",
    "stm32l1_cat5.toml",
    "stm32l1_cat6.toml",
]
CATEGORY_OF_FILE = {
    "stm32l1_cat1.toml": "cat1",
    "stm32l1_cat2.toml": "cat2",
    "stm32l1_cat3_die427.toml": "cat3a",
    "stm32l1_cat3_die436.toml": "cat3b",
    "stm32l1_cat4.toml": "cat4",
    "stm32l1_cat5.toml": "cat5",
    "stm32l1_cat6.toml": "cat6",
}
IP_LESS = {"FLASH", "PWR", "EXTI", "SYSCFG", "DBGMCU", "RI", "USB_SRAM"}
NOT_PERIPHERALS = {
    "NVIC",
    "SYS",
    "FATFS",
    "FREERTOS",
    "TS",
    "TOUCHSENSING",
    "USB_DEVICE",
    "I2S2",
    "I2S3",
}
IP_TO_SVD = {
    "COMP1": "COMP",
    "COMP2": "COMP",
    "OPAMP1": "OPAMP",
    "OPAMP2": "OPAMP",
    "OPAMP3": "OPAMP",
}
OWNER_OF_LINE = {
    0: "WWDG",
    1: "PWR",
    **dict.fromkeys((2, 6, 7, 8, 9, 10, 22, 23, 40), "EXTI"),
    **dict.fromkeys((3, 41), "RTC"),
    4: "FLASH",
    5: "RCC",
    **dict.fromkeys(range(11, 18), "DMA1"),
    18: "ADC",
    **dict.fromkeys((19, 20, 42), "USB"),
    21: "DAC",
    24: "LCD",
    25: "TIM9",
    26: "TIM10",
    27: "TIM11",
    28: "TIM2",
    29: "TIM3",
    30: "TIM4",
    31: "I2C1",
    32: "I2C1",
    33: "I2C2",
    34: "I2C2",
    35: "SPI1",
    36: "SPI2",
    37: "USART1",
    38: "USART2",
    39: "USART3",
    43: "TIM6",
    44: "TIM7",
    45: "SDIO",
    46: "TIM5",
    47: "SPI3",
    48: "UART4",
    49: "UART5",
    **dict.fromkeys(range(50, 55), "DMA2"),
    55: "AES",
    56: "COMP",
}

REPRESENTATIVE = {
    "cat1": "STM32L152RBTx",
    "cat2": "STM32L152RBTxA",
    "cat3a": "STM32L152RCTx",
    "cat3b": "STM32L152RCTxA",
    "cat4": "STM32L152ZDTx",
    "cat5": "STM32L152RETx",
    "cat6": "STM32L152VDTxX",
}
CMSIS_SUFFIX = {
    "cat1": "xb",
    "cat2": "xba",
    "cat3a": "xc",
    "cat3b": "xca",
    "cat4": "xd",
    "cat5": "xe",
    "cat6": "xdx",
}
DEV_ID = {
    "cat1": 0x416,
    "cat2": 0x429,
    "cat3a": 0x427,
    "cat3b": 0x436,
    "cat4": 0x436,
    "cat5": 0x437,
    "cat6": 0x437,
}


def _category(part: DbPart) -> str:
    flash = part.flash_kib[0]
    if part.die == "DIE416":
        return "cat1"
    if part.die == "DIE429":
        return "cat2"
    if part.die == "DIE427":
        return "cat3a"
    if part.die == "DIE436":
        return "cat3b" if flash == 256 else "cat4"
    return "cat5" if flash == 512 else "cat6"


@cache
def _parts() -> dict[str, DbPart]:
    pin_data = require_pin_data()
    parts: dict[str, DbPart] = {}
    for path in sorted((pin_data / "mcu").glob("STM32L1*.xml")):
        for name, _ in expand_refname(path.stem):
            parts[name] = read_part(path.read_bytes(), file=str(path), name=name)
    return parts


def _names() -> list[str]:
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        return []
    names: list[str] = []
    for path in sorted((Path(value) / "mcu").glob("STM32L1*.xml")):
        names.extend(name for name, _ in expand_refname(path.stem))
    return names


_sources = cache(find_sources)


@cache
def _built(name: str) -> PartDescription:
    return build_description(_sources(name))


def _patch_names(name: str) -> tuple[str, ...]:
    return tuple(patch.name for patch in _sources(name).patches)


def _peripherals(name: str) -> dict[str, PeripheralDesc]:
    return {p.name: p for p in _built(name).peripherals}


def _region(name: str, region: str) -> RegionDesc:
    return next(r for r in _built(name).regions if r.name == region)


def _register_reset(name: str, peripheral: str, register: str) -> int:
    found = next(r for r in _peripherals(name)[peripheral].registers if r.name == register)
    return found.reset


def _register_names(name: str, peripheral: str) -> list[str]:
    return [r.name for r in _peripherals(name)[peripheral].registers]


def _of_category(category: str) -> list[str]:
    return [n for n, p in _parts().items() if _category(p) == category]


def _line_numbers(name: str, category: str) -> set[int]:
    numbers = set(range(45))
    if category not in {"cat1", "cat2"}:
        numbers |= {46, 47, *range(50, 55), 56}
    if category == "cat4":
        numbers |= {45, 48, 49}
    if category in {"cat5", "cat6"}:
        numbers |= {48, 49}
    if name.startswith("STM32L100") and category == "cat3a":
        numbers -= {46}
    if name.startswith("STM32L151"):
        numbers -= {24}
    if name.startswith("STM32L162"):
        numbers |= {55}
    return numbers


def _check_patches(name: str, category: str) -> None:
    chosen = [file for file in _patch_names(name) if file in INDIVIDUAL]
    assert [CATEGORY_OF_FILE[file] for file in chosen] == [category]
    rd = "stm32l1_cat4_rd.toml" in _patch_names(name)
    assert rd == (category == "cat4" and "RD" in name)


def _check_peripherals(name: str, category: str) -> None:
    part = _parts()[name]
    peripherals = _peripherals(name)
    ports = {f"GPIO{pin[1]}" for pin in part.pins if pin[0] == "P" and pin[1:2].isalpha()}
    expected = set[str]()
    for ip in part.ips:
        if ip.instance in NOT_PERIPHERALS or ip.instance == "GPIO":
            continue
        if ip.instance == "DMA":
            expected.add("DMA1")
            if category not in {"cat1", "cat2"}:
                expected.add("DMA2")
            continue
        expected.add(IP_TO_SVD.get(ip.instance, ip.instance))
    have = set(peripherals)
    gpio = {p for p in have if p.startswith("GPIO")}
    assert ports <= have
    assert have - gpio - IP_LESS == expected, sorted((have - gpio - IP_LESS) ^ expected)
    assert have >= IP_LESS
    if name.startswith("STM32L100"):
        assert gpio == {"GPIOA", "GPIOB", "GPIOC", "GPIOD", "GPIOH"}
    elif category in {"cat1", "cat2", "cat3a"}:
        assert gpio == {"GPIOA", "GPIOB", "GPIOC", "GPIOD", "GPIOE", "GPIOH"}
    else:
        assert gpio == {f"GPIO{c}" for c in "ABCDEFGH"}
    assert ("FSMC" in have) == (category == "cat4" and "RD" not in name)
    assert ("SDIO" in have) == (category == "cat4")
    assert ("SPI3" in have) == (category not in {"cat1", "cat2"})
    assert ("AES" in have) == name.startswith("STM32L162")
    assert not have & {"USART4", "USART5"}


def _check_versions(name: str, category: str) -> None:
    peripherals = _peripherals(name)
    gpio_version, flash_version = {
        "cat1": ("STM32L1_gpio_cat13", "STM32L1_flash_cat1"),
        "cat2": ("STM32L1_gpio_cat13", "STM32L1_flash_cat2"),
        "cat3a": ("STM32L1_gpio_cat13", "STM32L1_flash_cat3"),
        "cat3b": ("STM32L1_gpio_cat13", "STM32L1_flash_cat3"),
        "cat4": ("STM32L1_gpio_cat46", "STM32L1_flash_cat46"),
        "cat5": ("STM32L1_gpio_cat46", "STM32L1_flash_cat5"),
        "cat6": ("STM32L1_gpio_cat46", "STM32L1_flash_cat46"),
    }[category]
    assert peripherals["FLASH"].ip_version == flash_version
    ports = [p for p in peripherals.values() if p.name.startswith("GPIO")]
    assert ports and {p.ip_version for p in ports} == {gpio_version}
    early = category in {"cat1", "cat2"}
    assert (peripherals["EXTI"].ip_version == "STM32L1_exti_23") == early
    assert (peripherals["SYSCFG"].ip_version == "STM32L1_exti_23") == early
    assert (peripherals["ADC"].ip_version == "STM32L1_adc_cat12") == early
    assert (peripherals["RTC"].ip_version == "STM32L1_rtc_cat1") == (category == "cat1")
    spi = {peripherals[p].ip_version for p in ("SPI1", "SPI2")}
    assert (spi == {"STM32L1_spi_cat2"}) == (category == "cat2")


def _check_registers(name: str, category: str) -> None:
    wrpr = {
        "cat1": ["WRPR1"],
        "cat2": ["WRPR1"],
        "cat3a": ["WRPR1", "WRPR2"],
        "cat3b": ["WRPR1", "WRPR2"],
        "cat4": ["WRPR1", "WRPR2", "WRPR3"],
        "cat5": ["WRPR1", "WRPR2", "WRPR3", "WRPR4"],
        "cat6": ["WRPR1", "WRPR2", "WRPR3"],
    }[category]
    assert [r for r in _register_names(name, "FLASH") if r.startswith("WRPR")] == wrpr
    backups = sum(1 for r in _register_names(name, "RTC") if r.startswith("BKP"))
    if name.startswith("STM32L100"):
        assert backups == 5
    else:
        assert backups == (20 if category in {"cat1", "cat2"} else 32)
    assert ("BRR" in _register_names(name, "GPIOA")) == (category in {"cat4", "cat5", "cat6"})
    assert _register_reset(name, "DBGMCU", "IDCODE") == 0x1000_0000 | DEV_ID[category]


def _check_brr_wrpr4_and_timers(name: str, category: str) -> None:
    peripherals = _peripherals(name)
    if category in {"cat4", "cat5", "cat6"}:
        for port, peripheral in peripherals.items():
            if len(port) == 5 and port.startswith("GPIO"):
                register = next(r for r in peripheral.registers if r.name == "BRR")
                assert (register.offset, register.width, register.reset) == (0x28, 32, 0)
                assert register.acc.value == "write_only"
                assert [(f.name, f.lsb, f.width) for f in register.fields] == [("BR", 0, 16)]
    if category == "cat5":
        register = next(r for r in peripherals["FLASH"].registers if r.name == "WRPR4")
        assert (register.offset, register.width, register.reset) == (0x88, 32, 0)
        assert register.acc.value == "read_write"
        assert [(f.name, f.lsb, f.width) for f in register.fields] == [("WRP4", 0, 32)]
    if category in {"cat1", "cat2"}:
        assert "TIM5" not in peripherals and "OR" not in _register_names(name, "TIM2")
        return
    if name.startswith("STM32L100"):
        assert "TIM5" not in peripherals
    else:
        assert _register_reset(name, "TIM5", "ARR") == 0xFFFF
    assert _register_reset(name, "SPI3", "CR2") == 0
    for timer, field in (("TIM2", "ITR1_RMP"), ("TIM3", "ITR2_RMP")):
        register = next(r for r in peripherals[timer].registers if r.name == "OR")
        assert (register.offset, register.width, register.reset) == (0x50, 16, 0)
        assert [(f.name, f.lsb, f.width) for f in register.fields] == [(field, 0, 1)]


def _check_memory(name: str, category: str) -> None:
    flash_kib = _parts()[name].flash_kib[0]
    size, base = (0x4C, 0x50) if category in {"cat1", "cat2"} else (0xCC, 0xD0)
    region = _region(name, "OPTION_FACTORY")
    assert (region.start, region.size) == (0x1FF8_0000, 0x400)
    assert region.controller == "FLASH" and region.acc.write is False
    pieces = {piece.offset: bytes(piece.data) for piece in region.content or []}
    code = {384: 0, 256: 1}[flash_kib] if category in {"cat3b", "cat4"} else flash_kib
    assert pieces.pop(size) == code.to_bytes(2, "little")
    assert pieces == {
        base: bytes.fromhex("14003200014e524f"),
        base + 0x14: bytes.fromhex("43504f50"),
    }
    system = _region(name, "SYSTEM_MEMORY")
    expected_size = 0x1000 if category in {"cat1", "cat2"} else 0x2000
    assert (system.start, system.size) == (0x1FF0_0000, expected_size)
    assert system.acc.execute and not system.acc.write
    has_bank2 = any(r.name == "Flash_Bank2" for r in _built(name).regions)
    assert has_bank2 == (category == "cat6")
    if category == "cat6":
        assert _region(name, "Flash").controller == "FLASH"
        assert _region(name, "Flash_Bank2").controller == "FLASH"


def _check_vector_table(name: str, category: str) -> None:
    built = _built(name)
    numbers = _line_numbers(name, category)
    assert {irq.number for irq in built.irqs} == numbers
    assert built.core.num_irqs == max(numbers) + 1
    for number in sorted(numbers):
        owners = [p.name for p in built.peripherals if number in p.irqs]
        assert owners == [OWNER_OF_LINE[number]], number


def _check_identity(name: str, category: str) -> None:
    patches = load_patches(
        [PATCH_DIR / p for p in _patch_names(name)],
        part=name,
        svd_name=_sources(name).svd.name,
        db_part=_parts()[name],
    )
    assert cmsis_suffix(patches) == CMSIS_SUFFIX[category]
    peripherals = _peripherals(name)
    if category in {"cat3b", "cat4", "cat5"} and not name.startswith("STM32L100"):
        opamp = peripherals["OPAMP"]
        assert (opamp.ip, opamp.ip_version) == ("OPAMP", "cmosf9_opamp_v1_0_Cube")


RCC_UNIT = re.compile(r"^(.*?)(RST|LPEN|EN)$")
RCC_UNIT_REGISTERS = {
    "AHBRSTR",
    "APB2RSTR",
    "APB1RSTR",
    "AHBENR",
    "APB2ENR",
    "APB1ENR",
    "AHBLPENR",
    "APB2LPENR",
    "APB1LPENR",
}
RCC_UNIT_OWNER = {"ADC1": "ADC", "FLITF": "FLASH"}
NO_RCC_UNIT = {"RCC", "EXTI", "IWDG", "RTC", "DBGMCU", "RI", "USB_SRAM", "OPAMP"}
FREEZE = re.compile(r"^DBG_(.*?)_(STOP|SMBUS_TIMEOUT)$")
LSE_CSS = {
    ("CIR", "LSECSSF"),
    ("CIR", "LSECSSIE"),
    ("CIR", "LSECSSC"),
    ("CSR", "LSECSSON"),
    ("CSR", "LSECSSD"),
}


def _fields(peripheral: PeripheralDesc) -> set[tuple[str, str]]:
    return {(r.name, f.name) for r in peripheral.registers for f in r.fields}


def _check_rcc_bits(name: str, category: str) -> None:
    peripherals = _peripherals(name)
    rcc = peripherals["RCC"]
    kinds: dict[str, set[str]] = {}
    for register in rcc.registers:
        if register.name in RCC_UNIT_REGISTERS:
            for bits in register.fields:
                match = RCC_UNIT.match(bits.name)
                assert match is not None, bits.name
                kinds.setdefault(match.group(1), set()).add(match.group(2))
    owners = {RCC_UNIT_OWNER.get(stem, stem) for stem in kinds if stem != "SRAM"}
    assert owners == set(peripherals) - NO_RCC_UNIT, sorted(
        owners ^ (set(peripherals) - NO_RCC_UNIT)
    )
    assert all(found == {"RST", "EN", "LPEN"} for stem, found in kinds.items() if stem != "SRAM")
    freeze = {
        match.group(1)
        for register in peripherals["DBGMCU"].registers
        for bits in register.fields
        if (match := FREEZE.match(bits.name)) is not None
    }
    assert freeze <= set(peripherals), sorted(freeze - set(peripherals))
    fields = _fields(rcc)
    assert (fields >= LSE_CSS) == (category != "cat1")
    assert not (LSE_CSS & fields) or category != "cat1"
    assert {("CSR", "OBLRSTF"), ("CSR", "LPWRRSTF"), ("CR", "RTCPRE")} <= fields
    rtcpre = next(
        f for r in rcc.registers if r.name == "CR" for f in r.fields if f.name == "RTCPRE"
    )
    assert (rtcpre.lsb, rtcpre.width) == (29, 2)
    assert (
        "DBG_RTC_STOP" in {f.name for r in peripherals["DBGMCU"].registers for f in r.fields}
    ) == (category != "cat1")


@pytest.mark.parametrize("name", _names())
def test_every_l1_name_follows_its_category(name: str) -> None:
    category = _category(_parts()[name])
    _check_patches(name, category)
    _check_peripherals(name, category)
    _check_versions(name, category)
    _check_registers(name, category)
    _check_brr_wrpr4_and_timers(name, category)
    _check_memory(name, category)
    _check_vector_table(name, category)
    _check_identity(name, category)
    _check_rcc_bits(name, category)


def test_the_132_names_fall_into_seven_categories_with_the_expected_counts() -> None:
    parts = _parts()
    assert len(parts) == 132
    categories = Counter(_category(part) for part in parts.values())
    assert set(categories) == set(CATEGORY_OF_FILE.values())
    assert sum(categories[c] for c in ("cat4", "cat5", "cat6")) == 33
    cat4 = _of_category("cat4")
    assert sum("RD" in name for name in cat4) == 6
    assert sum("RD" not in name for name in cat4) == 9


def test_no_l1_patch_names_the_third_operational_amplifier() -> None:
    named = {path.name for path in PATCH_DIR.glob("stm32l1*.toml") if "OPAMP3" in path.read_text()}
    assert named == set()


@pytest.mark.parametrize(
    ("name", "count"),
    [
        ("STM32L152RBTx", 45),
        ("STM32L151RBTx", 44),
        ("STM32L100C6Ux", 45),
        ("STM32L152RCTx", 53),
        ("STM32L162RCTx", 54),
        ("STM32L152RCTxA", 53),
        ("STM32L152ZDTx", 56),
        ("STM32L162ZDTx", 57),
        ("STM32L152RETx", 55),
        ("STM32L151VDTxX", 54),
        ("STM32L152VDTxX", 55),
    ],
)
def test_the_vector_table_counts_of_each_category(name: str, count: int) -> None:
    assert len(_built(name).irqs) == count


def test_cat3_keeps_the_cat4_numbering_with_gaps() -> None:
    built = _built("STM32L152RCTx")
    numbers = {irq.number for irq in built.irqs}
    assert {46, 47, 50, 56} <= numbers
    assert numbers & {45, 48, 49} == set()
    assert built.core.num_irqs == 57
    assert _built("STM32L152RBTx").core.num_irqs == 45


def test_the_cat3_die436_part_keeps_the_aes_of_the_l162_only() -> None:
    assert "AES" not in _peripherals("STM32L152RCTxA")
    assert _peripherals("STM32L162RCTxA")["AES"].ip == "AES"
    assert "LCD" not in _peripherals("STM32L151RBTx")
    assert "LCD" in _peripherals("STM32L152RBTx")


def test_the_tim5_fields_are_corrected_where_tim5_exists() -> None:
    tim5 = _peripherals("STM32L152RETx")["TIM5"]
    widths = {
        (r.name, f.name): f.width
        for r in tim5.registers
        for f in r.fields
        if f.name in {"CC4S", "CC2S"}
    }
    assert widths[("CCMR2_Output", "CC4S")] == 2
    assert widths[("CCMR1_Output", "CC2S")] == 2


def _header_irqs(name: str) -> dict[int, str]:
    header = _sources(name).cmsis_header
    if header is None:
        pytest.skip("the L1 CMSIS device Include directory is not in POPCORN_CMSIS_DEVICE_DIRS")
    return read_cmsis_irqs(header.read_text(encoding="utf-8"), file=str(header))


@pytest.mark.parametrize("name", _names())
def test_every_l1_name_takes_its_irqs_from_the_header_of_its_line_and_category(name: str) -> None:
    category = _category(_parts()[name])
    header_irqs = _header_irqs(name)
    sources = _sources(name)
    assert sources.cmsis_header is not None
    assert sources.cmsis_header.name == f"stm32l{name[6:9]}{CMSIS_SUFFIX[category]}.h"
    built = _built(name)
    assert {irq.number: irq.name for irq in built.irqs} == header_irqs
    assert (
        built.sources["cmsis_header"]
        == hashlib.sha256(sources.cmsis_header.read_bytes()).hexdigest()
    )


@pytest.mark.parametrize(
    ("name", "lcd"),
    [
        ("STM32L100C6Ux", "LCD_IRQn"),
        ("STM32L100RCTx", "LCD_IRQn"),
        ("STM32L151RBTx", None),
        ("STM32L151VDTxX", None),
        ("STM32L152RBTx", "LCD_IRQn"),
        ("STM32L162RETx", "LCD_IRQn"),
    ],
)
def test_the_lcd_line_follows_the_header_of_each_line(name: str, lcd: str | None) -> None:
    assert _header_irqs(name).get(24) == lcd
    owners = [p.name for p in _built(name).peripherals if 24 in p.irqs]
    assert owners == (["LCD"] if lcd else [])
    assert ("LCD" in _peripherals(name)) == (lcd is not None)
