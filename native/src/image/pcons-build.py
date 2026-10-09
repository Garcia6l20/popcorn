from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / "arm_attributes.cpp",
    here / "digest.cpp",
    here / "dwarf_lines.cpp",
    here / "dwarf_macros.cpp",
    here / "dwarf_reader.cpp",
    here / "dwarf_units.cpp",
    here / "elf_file.cpp",
    here / "image.cpp",
    here / "load_elf.cpp",
    here / "load_raw.cpp",
    here / "read_macros.cpp",
]
