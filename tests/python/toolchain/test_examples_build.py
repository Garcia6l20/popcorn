from pathlib import Path

import pytest

from popcorn_build.examples import offset_linker_script, prebuilt_elf

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
