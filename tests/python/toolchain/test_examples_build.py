from pathlib import Path

import pytest

from popcorn_build.examples import (
    TrustZoneLayout,
    nonsecure_linker_script,
    offset_linker_script,
    prebuilt_elf,
    secure_linker_script,
)

ST_MEMORY = """MEMORY
{
  RAM\t(xrw)\t: ORIGIN = 0x20000000,\tLENGTH = 768K
  ROM\t(rx)\t: ORIGIN = 0x08000000,\tLENGTH = 2048K
  SRAM4\t(xrw)\t: ORIGIN = 0x28000000,\tLENGTH = 16K
}
SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(8);
    KEEP(*(.isr_vector))
    . = ALIGN(8);
  } >ROM
}
"""
HEADER_FIRST = "    KEEP(*(.app_header))\n    KEEP(*(.isr_vector))\n"


def test_an_example_prebuilt_elf_sits_in_its_own_prebuilt_directory() -> None:
    example = Path("examples") / "stm32u575" / "spi"
    assert prebuilt_elf(example, "spi_flash") == example / "prebuilt" / "spi_flash.elf"


def test_a_flash_offset_moves_the_rom_region_and_puts_the_app_header_first() -> None:
    derived = offset_linker_script(ST_MEMORY, 0xC000)
    assert "ROM\t(rx)\t: ORIGIN = 0x0800c000,\tLENGTH = 2000K" in derived
    assert HEADER_FIRST in derived
    restored = derived.replace("0x0800c000,\tLENGTH = 2000K", "0x08000000,\tLENGTH = 2048K")
    assert restored.replace("    KEEP(*(.app_header))\n", "") == ST_MEMORY


@pytest.mark.parametrize("offset", [0, -0x400, 0x200])
def test_a_flash_offset_is_a_positive_multiple_of_1_kib(offset: int) -> None:
    with pytest.raises(ValueError, match="multiple of 1 KiB"):
        offset_linker_script(ST_MEMORY, offset)


def test_a_script_without_the_st_rom_region_is_refused() -> None:
    with pytest.raises(ValueError, match="no single ROM region"):
        offset_linker_script(ST_MEMORY.replace("0x08000000", "0x08100000"), 0xC000)


def test_a_script_without_a_single_vector_table_section_is_refused() -> None:
    with pytest.raises(ValueError, match="no single KEEP"):
        offset_linker_script(ST_MEMORY.replace("KEEP(*(.isr_vector))", ""), 0xC000)


ST_SECURE_MEMORY = """MEMORY
{
  RAM\t(xrw)\t: ORIGIN = 0x30000000,\tLENGTH = 256K     /* Memory is divided. */
  ROM\t(rx)\t: ORIGIN = 0x0C000000,\tLENGTH = 1016K    /* Memory is divided. */
  ROM_NSC\t(rx)\t: ORIGIN = 0x0C0FE000,\tLENGTH = 8K   /* Non-Secure Call-able region */
  SRAM4\t(xrw)\t: ORIGIN = 0x38000000,\tLENGTH = 16K      /* Secure SRAM4 */
}
SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } >ROM
}
"""
LAYOUT = TrustZoneLayout(
    secure_flash=(0x0800_0000, 0x8000),
    nsc_size=0x400,
    secure_ram=(0x2800_0000, 0x4000),
    nonsecure_flash=(0x0800_8000, 0x3_8000),
    nonsecure_ram=(0x2000_0000, 0x2_FE00),
)


def test_the_secure_script_moves_its_regions_and_exports_the_nonsecure_ranges() -> None:
    derived = secure_linker_script(ST_SECURE_MEMORY, LAYOUT)
    assert "  RAM\t(xrw)\t: ORIGIN = 0x28000000,\tLENGTH = 0x4000\n" in derived
    assert "  ROM\t(rx)\t: ORIGIN = 0x08000000,\tLENGTH = 0x7c00\n" in derived
    assert "  ROM_NSC\t(rx)\t: ORIGIN = 0x08007c00,\tLENGTH = 0x400\n" in derived
    assert "SRAM4" not in derived
    assert "Memory is divided" not in derived
    assert "__nonsecure_flash_start = 0x08008000;" in derived
    assert "__nonsecure_flash_end = 0x08040000;" in derived
    assert "__nonsecure_ram_end = 0x2002fe00;" in derived
    assert "__sgstubs_start = ORIGIN(ROM_NSC);" in derived
    assert "ASSERT(ADDR(.gnu.sgstubs) == ORIGIN(ROM_NSC)" in derived


def test_the_nonsecure_script_places_the_secure_image_first_in_its_own_region() -> None:
    derived = nonsecure_linker_script(ST_MEMORY, LAYOUT)
    assert "MEMORY\n{\n  SECURE\t(rx)\t: ORIGIN = 0x08000000,\tLENGTH = 0x8000\n" in derived
    assert "  ROM\t(rx)\t: ORIGIN = 0x08008000,\tLENGTH = 0x38000\n" in derived
    assert "  RAM\t(xrw)\t: ORIGIN = 0x20000000,\tLENGTH = 0x2fe00\n" in derived
    assert "SRAM4" not in derived
    image = derived.index("  .secure_image :\n  {\n    KEEP(*(.secure_image))\n  } >SECURE\n")
    assert image < derived.index("  .isr_vector :")
    assert 'ASSERT(ADDR(.secure_image) == ORIGIN(SECURE), "' in derived
    assert "ASSERT(SIZEOF(.secure_image) <= LENGTH(SECURE)" in derived


def test_a_script_without_a_named_region_is_refused() -> None:
    with pytest.raises(ValueError, match="no single MEMORY region ROM_NSC"):
        secure_linker_script(ST_MEMORY, LAYOUT)


def test_a_nonsecure_script_without_a_single_vector_section_is_refused() -> None:
    with pytest.raises(ValueError, match=r"no single \.isr_vector output section"):
        nonsecure_linker_script(ST_MEMORY.replace(".isr_vector :", ".text :"), LAYOUT)


@pytest.mark.parametrize("nsc_size", [0, 0x30, 0x8000])
def test_a_veneer_range_is_a_multiple_of_the_sau_granule_inside_the_secure_flash(
    nsc_size: int,
) -> None:
    with pytest.raises(ValueError, match="multiple of 32 bytes"):
        TrustZoneLayout(
            secure_flash=(0x0800_0000, 0x8000),
            nsc_size=nsc_size,
            secure_ram=(0x2800_0000, 0x4000),
            nonsecure_flash=(0x0800_8000, 0x3_8000),
            nonsecure_ram=(0x2000_0000, 0x2_FE00),
        )


def test_an_empty_range_is_refused() -> None:
    with pytest.raises(ValueError, match="the secure_ram range is empty"):
        TrustZoneLayout(
            secure_flash=(0x0800_0000, 0x8000),
            nsc_size=0x400,
            secure_ram=(0x2800_0000, 0),
            nonsecure_flash=(0x0800_8000, 0x3_8000),
            nonsecure_ram=(0x2000_0000, 0x2_FE00),
        )


@pytest.mark.parametrize(
    ("bound", "layout"),
    [
        ("secure_flash end", {"secure_flash": (0x0800_0000, 0x8010)}),
        ("nonsecure_flash start", {"nonsecure_flash": (0x0800_8010, 0x3_7FF0)}),
        ("nonsecure_flash end", {"nonsecure_flash": (0x0800_8000, 0x3_7FF0)}),
        ("nonsecure_ram start", {"nonsecure_ram": (0x2000_0010, 0x2_FDF0)}),
        ("nonsecure_ram end", {"nonsecure_ram": (0x2000_0000, 0x2_FE10)}),
    ],
)
def test_a_bound_the_sau_sets_is_a_multiple_of_its_granule(
    bound: str, layout: dict[str, tuple[int, int]]
) -> None:
    ranges = {
        "secure_flash": (0x0800_0000, 0x8000),
        "secure_ram": (0x2800_0000, 0x4000),
        "nonsecure_flash": (0x0800_8000, 0x3_8000),
        "nonsecure_ram": (0x2000_0000, 0x2_FE00),
    } | layout
    with pytest.raises(ValueError, match=f"the {bound} is a multiple of the 32-byte SAU granule"):
        TrustZoneLayout(nsc_size=0x400, **ranges)


def test_the_nonsecure_ram_end_is_a_multiple_of_the_mpcbb_block() -> None:
    with pytest.raises(ValueError, match="nonsecure_ram end is a multiple of the 512-byte MPCBB"):
        TrustZoneLayout(
            secure_flash=(0x0800_0000, 0x8000),
            nsc_size=0x400,
            secure_ram=(0x2800_0000, 0x4000),
            nonsecure_flash=(0x0800_8000, 0x3_8000),
            nonsecure_ram=(0x2000_0000, 0x2_FE20),
        )
