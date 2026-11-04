"""The STM32L1 rzone facts files and the family patch `stm32l1.toml`, over the pin data."""

import json
import tomllib
from functools import cache
from pathlib import Path
from typing import Any, cast

import pytest
from popcorn.stm32.description import _cubemx
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cubemx import expand_refname, read_rzone_facts
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._sources import facts_path, find_sources, read_db_part

from ..conftest import require_pin_data

FACTS_DIR = Path(_cubemx.__file__).parent / "rzone"
PATCH = FACTS_DIR.parent / "patches" / "stm32l1.toml"

PAIRS: dict[str, list[tuple[int, int]]] = {
    "DIE416": [(4, 32), (8, 64), (10, 32), (10, 64), (10, 128), (16, 128)],
    "DIE429": [(4, 32), (8, 64), (16, 32), (16, 128), (32, 64), (32, 128)],
    "DIE427": [(16, 256), (32, 256)],
    "DIE436": [(32, 256), (48, 384)],
    "DIE437": [(80, 384), (80, 512)],
}
STEMS = [f"STM32_{die}_{ram}_{flash}" for die, rows in PAIRS.items() for ram, flash in rows]
INTERRUPTS = {"DIE416": 45, "DIE429": 45, "DIE427": 57, "DIE436": 57, "DIE437": 57}
LINE_PARTS = ["STM32L100RCTx", "STM32L151RBTx", "STM32L152RETx", "STM32L162ZDTx"]


def _facts(stem: str) -> dict[str, Any]:
    return tomllib.loads((FACTS_DIR / f"{stem}.toml").read_text(encoding="utf-8"))


def _die(stem: str) -> str:
    return stem.split("_")[1]


def _ram_flash(stem: str) -> tuple[int, int]:
    _, _, ram, flash = stem.split("_")
    return int(ram), int(flash)


def test_the_l1_facts_files_are_the_eighteen_pairs_of_the_plan() -> None:
    present = sorted(
        path.stem for path in FACTS_DIR.glob("STM32_DIE4*.toml") if _die(path.stem) in PAIRS
    )
    assert present == sorted(STEMS)
    assert len(STEMS) == 18


@pytest.mark.parametrize("stem", STEMS)
def test_a_facts_file_reads_and_carries_the_core_of_a_cortex_m3(stem: str) -> None:
    path = FACTS_DIR / f"{stem}.toml"
    rzone = read_rzone_facts(path.read_bytes(), file=str(path))
    assert _facts(stem)["rzone"] == f"{stem}.xml"
    processor = rzone.processor
    assert processor.fpu is None
    assert (processor.dsp, processor.tz) == (False, False)
    assert (processor.mpu_regions, processor.sec_mpu_regions, processor.sau_regions) == (8, 0, 0)
    assert processor.num_interrupts == INTERRUPTS[_die(stem)]


@pytest.mark.parametrize("stem", STEMS)
def test_a_facts_file_maps_its_sram_and_flash_from_its_name(stem: str) -> None:
    ram, flash = _ram_flash(stem)
    path = FACTS_DIR / f"{stem}.toml"
    regions = {r.name: r for r in read_rzone_facts(path.read_bytes(), file=str(path)).regions}
    assert (regions["SRAM"].start, regions["SRAM"].size) == (0x2000_0000, ram * 1024)
    assert regions["SRAM"].acc.execute and regions["SRAM"].acc.write
    flash_regions = [r for name, r in regions.items() if name.startswith("Flash")]
    assert all(r.start >= 0x0800_0000 and not r.acc.write and r.acc.execute for r in flash_regions)
    assert sum(r.size for r in flash_regions) == flash * 1024
    if stem.startswith("STM32_DIE437") and flash == 384:
        assert [(r.name, r.start, r.size) for r in flash_regions] == [
            ("Flash", 0x0800_0000, 0x30000),
            ("Flash_Bank2", 0x0804_0000, 0x30000),
        ]
    else:
        assert [(r.name, r.start, r.size) for r in flash_regions] == [
            ("Flash", 0x0800_0000, flash * 1024)
        ]


def test_every_l1_name_of_the_db_selects_one_of_the_facts_files() -> None:
    pin_data = require_pin_data()
    seen: set[str] = set()
    count = 0
    for path in sorted((pin_data / "mcu").glob("STM32L1*.xml")):
        for name, _ in expand_refname(path.stem):
            part = read_db_part(path, name)
            stem = facts_path(part).stem
            assert (FACTS_DIR / f"{stem}.toml").is_file(), name
            assert _die(stem) == part.die
            assert _ram_flash(stem) == (part.ram_kib[0], part.flash_kib[0])
            seen.add(stem)
            count += 1
    assert count == 132
    assert seen == set(STEMS)


@cache
def _description(name: str) -> dict[str, Any]:
    require_pin_data()
    try:
        sources = find_sources(name)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    assert sources.rzone is None and sources.rzone_facts is not None
    return cast(dict[str, Any], json.loads(dump_json(build_description(sources))))


def _named(description: dict[str, Any], key: str) -> dict[str, Any]:
    return {item["name"]: item for item in description[key]}


@pytest.mark.timeout(120)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_family_patch_applies_to_each_svd_line(name: str) -> None:
    description = _description(name)
    peripherals = _named(description, "peripherals")
    assert "FLASH" in peripherals and "Flash" not in peripherals
    for port in (p for p in peripherals if len(p) == 5 and p.startswith("GPIO")):
        registers = [r["name"] for r in peripherals[port]["registers"]]
        assert "OSPEEDR" in registers and "OSPEEDER" not in registers, port
    assert description["core"]["reset_clock_hz"] == 2_097_152
    assert description["core"]["mpu"] is True
    assert description["core"]["mpu_regions"] == 8


@pytest.mark.timeout(120)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_the_family_patch_corrects_the_reset_values_and_field_widths(name: str) -> None:
    peripherals = _named(_description(name), "peripherals")

    def register(peripheral: str, register: str) -> dict[str, Any]:
        return next(r for r in peripherals[peripheral]["registers"] if r["name"] == register)

    assert register("RCC", "APB2LPENR")["reset"] == 0x521D
    assert register("RCC", "APB1LPENR")["reset"] == 0xB0E64A37
    for spi in ("SPI1", "SPI2"):
        assert register(spi, "CR2")["reset"] == 0
    for timer in ("TIM2", "TIM3", "TIM4", "TIM6", "TIM7", "TIM9", "TIM10", "TIM11"):
        assert register(timer, "ARR")["reset"] == 0xFFFF, timer
    for timer in ("TIM2", "TIM3", "TIM4"):
        cc4s = next(f for f in register(timer, "CCMR2_Output")["fields"] if f["name"] == "CC4S")
        assert cc4s["width"] == 2
    for timer in ("TIM2", "TIM3", "TIM4", "TIM9"):
        cc2s = next(f for f in register(timer, "CCMR1_Output")["fields"] if f["name"] == "CC2S")
        assert cc2s["width"] == 2, timer
    length = next(f for f in register("ADC", "SQR1")["fields"] if f["name"] == "L")
    assert (length["lsb"], length["width"]) == (20, 5)


@pytest.mark.timeout(120)
@pytest.mark.parametrize("name", LINE_PARTS)
def test_usb_sram_is_a_registerless_window_and_one_peripheral_has_the_usb_registers(
    name: str,
) -> None:
    peripherals = _named(_description(name), "peripherals")
    sram = peripherals["USB_SRAM"]
    assert (sram["base"], sram["size"], sram["registers"]) == (0x4000_6000, 0x400, [])
    assert sram["ip"] != "USB"
    holders = [
        p["name"]
        for p in peripherals.values()
        if any(r["name"] in ("EP0R", "USB_EP0R") for r in p["registers"])
    ]
    assert holders == ["USB"]
    assert [p["name"] for p in peripherals.values() if p["ip"] == "USB"] == ["USB"]


@pytest.mark.timeout(120)
@pytest.mark.parametrize(
    ("name", "stem"),
    [
        ("STM32L152RBTx", "STM32_DIE416_16_128"),
        ("STM32L152RETx", "STM32_DIE437_80_512"),
        ("STM32L151VDTxX", "STM32_DIE437_80_384"),
    ],
)
def test_the_family_patch_adds_the_bit_band_windows_the_data_eeprom_and_the_boot_alias(
    name: str, stem: str
) -> None:
    description = _description(name)
    regions = _named(description, "regions")
    assert "rzone_facts" in description["sources"]
    assert (regions["BB_SRAM"]["start"], regions["BB_SRAM"]["physical"]) == (
        0x2200_0000,
        0x2000_0000,
    )
    assert (regions["BB_PERIPH"]["start"], regions["BB_PERIPH"]["physical"]) == (
        0x4200_0000,
        0x4000_0000,
    )
    assert {regions[n]["kind"] for n in ("BB_SRAM", "BB_PERIPH")} == {"bit_band"}
    assert regions["DATA_EEPROM"]["start"] == 0x0808_0000
    assert regions["DATA_EEPROM"]["controller"] == "FLASH"
    assert regions["Flash"]["controller"] == "FLASH"
    ram, flash = _ram_flash(stem)
    assert regions["SRAM"]["size"] == ram * 1024
    assert sum(r["size"] for n, r in regions.items() if n.startswith("Flash")) == flash * 1024
    banks = sorted(
        (r for n, r in regions.items() if n.startswith("Flash")), key=lambda r: r["start"]
    )
    boots = sorted(
        (r for n, r in regions.items() if n.startswith("BOOT")), key=lambda r: r["start"]
    )
    assert [(r["start"], r["size"], r["physical"], r["kind"]) for r in boots] == [
        (r["start"] - 0x0800_0000, r["size"], r["start"], r["kind"]) for r in banks
    ]
    assert all("controller" not in r for r in boots)


def test_a_facts_file_refuses_a_missing_mpu_secure_count(tmp_path: Path) -> None:
    text = (FACTS_DIR / f"{STEMS[0]}.toml").read_text(encoding="utf-8")
    broken = text.replace("mpu_regions_secure = 0\n", "")
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone_facts(broken.encode(), file="broken.toml")
    assert excinfo.value.key == "core.mpu_regions_secure"
