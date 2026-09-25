from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / "arm_attributes.cpp",
    here / "digest.cpp",
    here / "elf_file.cpp",
    here / "image.cpp",
    here / "load_elf.cpp",
    here / "raw_stub.cpp",
]
