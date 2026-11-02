"""The STM32L4 DIE415 rzone facts files and the family patch `stm32l4.toml`, over the pin data."""

import tomllib
from functools import cache
from pathlib import Path

import pytest
from popcorn.stm32.description import _cubemx
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cubemx import expand_refname, read_rzone_facts
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._model import PartDescription, PeripheralDesc, RegionDesc
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
