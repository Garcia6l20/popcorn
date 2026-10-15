from pathlib import Path

here = Path(__file__).resolve().parent
sources = [here / name for name in ("loader.cpp", "elf_section.cpp", "guard.cpp")]
