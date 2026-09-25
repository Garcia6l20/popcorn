from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import (
    DbPart,
    Rzone,
    instance_ips,
    read_part,
    read_rzone,
    rzone_file_name,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import RegionKind
from popcorn.stm32.description._svd import expand_svd

DATA_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "db"


def _region(rzone: Rzone, name: str):
    for region in rzone.regions:
        if region.name == name:
            return region
    raise KeyError(name)


def test_synthetic_part_die_ram_flash_and_ips() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    assert part.ref_name == "STM32T001ABCx"
    assert part.family == "STM32T0"
    assert part.line == "STM32T001"
    assert part.die == "DIE000"
    assert part.ram_kib == [16, 4]
    assert part.flash_kib == [64]
    assert {ip.instance for ip in part.ips} == {"USART1", "USB_OTG_FS", "GPIOA"}


def test_synthetic_rzone_file_name_uses_first_ram_and_flash() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    assert rzone_file_name(part) == "STM32_DIE000_16_64.xml"


def test_namespace_less_part_is_a_description_error() -> None:
    data = b"""<?xml version="1.0"?>
    <Mcu RefName="X"><Die>DIE000</Die><Ram>16</Ram><Flash>64</Flash></Mcu>
    """
    with pytest.raises(DescriptionError) as excinfo:
        read_part(data, file="nons.xml")
    assert excinfo.value.key == "Mcu"


def test_malformed_part_xml_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_part(b"<Mcu><Die>DIE000</Die>", file="broken.xml")
    assert excinfo.value.key == "<root>"


def test_synthetic_rzone_processor_and_regions() -> None:
    data = (DATA_DIR / "mcu" / "memory" / "STM32_DIE000_16_64.xml").read_bytes()
    rzone = read_rzone(data, file="STM32_DIE000_16_64.xml")
    assert rzone.processor.clock_hz == 80_000_000
    assert rzone.processor.core == "ARM Cortex-M0"
    assert rzone.processor.core_version == "r0p0"
    assert rzone.processor.fpu is None
    assert rzone.processor.dsp is False
    assert rzone.processor.mpu is True
    assert rzone.processor.tz is False
    assert rzone.processor.num_interrupts == 32
    assert rzone.processor.mpu_regions == 8
    assert rzone.processor.sec_mpu_regions == 0
    assert rzone.processor.sau_regions == 0
    assert len(rzone.regions) == 4

    flash = _region(rzone, "Flash_NS")
    assert flash.start == 0x08000000
    assert flash.size == 0x00010000
    assert flash.kind == RegionKind.ROM
    assert flash.acc.read is True
    assert flash.acc.write is False
    assert flash.acc.execute is True
    assert flash.secure is False
    assert flash.physical is None

    alias = _region(rzone, "SRAM_ALIAS")
    assert alias.physical == 0x20000000
    assert alias.secure is False
    assert alias.acc.read is True
    assert alias.acc.write is True
    assert alias.acc.execute is True

    secure = _region(rzone, "SRAM_S")
    assert secure.secure is True
    assert secure.physical is None

    rw_only = _region(rzone, "BKPSRAM_NS")
    assert rw_only.acc.read is True
    assert rw_only.acc.write is True
    assert rw_only.acc.execute is False
    assert rw_only.physical is None
    assert rw_only.secure is False


def test_unsupported_memory_type_is_a_description_error() -> None:
    data = (DATA_DIR / "mcu" / "memory" / "STM32_DIE000_bad.xml").read_bytes()
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone(data, file="STM32_DIE000_bad.xml")
    assert excinfo.value.key == "memory[OTP_AREA].type"


def test_malformed_rzone_xml_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone(b"<rzone><resources>", file="broken.xml")
    assert excinfo.value.key == "<root>"


def test_instance_ips_exact_match_then_rename() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    result = instance_ips(
        part,
        svd_names={"USART1", "OTG_FS", "GPIOA", "UNKNOWN"},
        renames={"USB_OTG_FS": ["OTG_FS"]},
    )
    assert result.keys() == {"USART1", "OTG_FS", "GPIOA"}
    assert result["USART1"].instance == "USART1"
    assert result["USART1"].name == "USART"
    assert result["OTG_FS"].instance == "USB_OTG_FS"
    assert result["OTG_FS"].name == "USB_OTG"
    assert "UNKNOWN" not in result


@pytest.fixture(scope="session")
def u575_part(st_cubemx_db: Path) -> DbPart:
    data = (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes()
    return read_part(data, file="STM32U575QGIx.xml")


@pytest.fixture(scope="session")
def u575_rzone(st_cubemx_db: Path, u575_part: DbPart) -> Rzone:
    file_name = rzone_file_name(u575_part)
    data = (st_cubemx_db / "mcu" / "memory" / file_name).read_bytes()
    return read_rzone(data, file=file_name)


def test_u575_part_and_rzone_file_name(u575_part: DbPart) -> None:
    assert u575_part.ref_name == "STM32U575QGIx"
    assert u575_part.die == "DIE482"
    assert u575_part.ram_kib == [786]
    assert u575_part.flash_kib == [1024]
    assert rzone_file_name(u575_part) == "STM32_DIE482_786_1024.xml"
    assert len(u575_part.ips) == 110


def test_u575_processor(u575_rzone: Rzone) -> None:
    processor = u575_rzone.processor
    assert processor.clock_hz == 160_000_000
    assert processor.num_interrupts == 125
    assert processor.sau_regions == 8
    assert processor.fpu == "SP_FPU"
    assert processor.tz is True
    assert processor.mpu_regions == 16
    assert processor.sec_mpu_regions == 8


def test_u575_ten_regions_match_i05(u575_rzone: Rzone) -> None:
    expected = {
        "Flash_NS": (0x08000000, 0x00100000, "rx", False, None, RegionKind.ROM),
        "RAM_C_NS": (0x0A000000, 0x000C0000, "rwx", False, 0x20000000, RegionKind.RAM),
        "Flash_S": (0x0C000000, 0x00100000, "rx", True, 0x08000000, RegionKind.ROM),
        "RAM_C_S": (0x0E000000, 0x000C0000, "rwx", True, 0x20000000, RegionKind.RAM),
        "SRAM_NS": (0x20000000, 0x000C0000, "rwx", False, None, RegionKind.RAM),
        "SRAM_SRD_NS": (0x28000000, 0x00004000, "rwx", False, None, RegionKind.RAM),
        "SRAM_S": (0x30000000, 0x000C0000, "rwx", True, 0x20000000, RegionKind.RAM),
        "SRAM_SRD_S": (0x38000000, 0x00004000, "rwx", True, 0x28000000, RegionKind.RAM),
        "BKPSRAM_NS": (0x40036400, 0x00000800, "rw", False, None, RegionKind.RAM),
        "BKPSRAM_S": (0x50036400, 0x00000800, "rw", True, 0x40036400, RegionKind.RAM),
    }
    assert len(u575_rzone.regions) == 10
    assert {r.name for r in u575_rzone.regions} == expected.keys()
    for region in u575_rzone.regions:
        start, size, access, secure, physical, kind = expected[region.name]
        assert region.start == start
        assert region.size == size
        assert region.acc.read == ("r" in access)
        assert region.acc.write == ("w" in access)
        assert region.acc.execute == ("x" in access)
        assert region.secure == secure
        assert region.physical == physical
        assert region.kind == kind


_U575_RENAMES = {
    "USB_OTG_FS": ["OTG_FS"],
    "DCACHE1": ["DCACHE"],
    "ICACHE": ["ICache"],
    "COMP1": ["COMP"],
    "COMP2": ["COMP"],
    "OPAMP1": ["OPAMP"],
    "OPAMP2": ["OPAMP"],
    "GPIO": [
        "GPIOA",
        "GPIOB",
        "GPIOC",
        "GPIOD",
        "GPIOE",
        "GPIOF",
        "GPIOG",
        "GPIOH",
        "GPIOI",
        "LPGPIO1",
    ],
    "GTZC": ["GTZC1_TZSC", "GTZC1_TZIC"],
    "GTZC_NS": ["GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3"],
    "GTZC_S": ["GTZC2_TZSC", "GTZC2_TZIC", "GTZC2_MPCBB4"],
    "SYS": ["SYSCFG"],
}


def test_u575_ip_join_unmatched_peripherals_match_i05(st_svd_u575: Path, u575_part: DbPart) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    svd_names = {p.name for p in device.peripherals}
    result = instance_ips(u575_part, svd_names, _U575_RENAMES)
    unmatched = svd_names - result.keys()
    non_sec_unmatched = {name for name in unmatched if not name.startswith("SEC_")}
    sec_unmatched = {name for name in unmatched if name.startswith("SEC_")}

    assert non_sec_unmatched == {
        "ADC12",
        "CRS",
        "DBGMCU",
        "DCB",
        "DLYBOS1",
        "DLYBOS2",
        "DLYBSD1",
        "DLYBSD2",
        "EXTI",
        "FDCAN1_RAM",
        "OTFDEC1",
        "OTFDEC2",
        "PKA",
    }
    assert sec_unmatched == {name for name in svd_names if name.startswith("SEC_")}
    assert result["USART1"].name == "USART"
    assert result["OTG_FS"].instance == "USB_OTG_FS"
