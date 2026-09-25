"""Generates golden addr2line output for the image loader's line-table tests.

Probes every DWARF line-table row address and every halfword of every function symbol
in an ELF, resolves each through `arm-none-eabi-addr2line`, and writes one
`<hex address> <file>:<line>` row per line, path normalized the way the C++ decoder
normalizes it (no `./`, no `..`, forward slashes).
"""

from __future__ import annotations

import posixpath
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

FIRMWARE_NAMES = ("image", "image-dw4", "image-dw5", "image-dw5gcc")


def _probe_addresses(elf_path: Path) -> list[int]:
    addrs: set[int] = set()
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        symtab = elf.get_section_by_name(".symtab")
        if isinstance(symtab, SymbolTableSection):
            for sym in symtab.iter_symbols():
                if sym["st_info"]["type"] != "STT_FUNC":
                    continue
                value = sym["st_value"] & ~1
                size = sym["st_size"]
                if size == 0:
                    addrs.add(value)
                else:
                    addrs.update(range(value, value + size, 2))

        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            line_program = dwarf.line_program_for_CU(cu)
            if line_program is None:
                continue
            for entry in line_program.get_entries():
                if entry.state is not None:
                    addrs.add(entry.state.address)
    return sorted(addrs)


def _normalize(path: str) -> str:
    return posixpath.normpath(path)


def _resolve(elf_path: Path, addresses: list[int], addr2line: str) -> list[str]:
    if not addresses:
        return []
    stdin_text = "\n".join(f"{addr:#x}" for addr in addresses)
    result = subprocess.run(
        [addr2line, "-e", str(elf_path)],
        input=stdin_text,
        capture_output=True,
        text=True,
        check=True,
    )
    lines: list[str] = []
    for raw_line in result.stdout.splitlines():
        text = raw_line.split(" (discriminator", 1)[0].strip()
        if text == "??:0" or text == "??:?":
            lines.append("??:0")
            continue
        file_part, _, line_part = text.rpartition(":")
        lines.append(f"{_normalize(file_part)}:{line_part}")
    return lines


def generate_golden(elf_path: Path, addr2line: str = "arm-none-eabi-addr2line") -> str:
    """Golden `locate` output for `elf_path`, one `<hex address> <file>:<line>` row per line."""
    addresses = _probe_addresses(elf_path)
    resolved = _resolve(elf_path, addresses, addr2line)
    return "".join(f"{addr:08x} {loc}\n" for addr, loc in zip(addresses, resolved, strict=True))


def write_golden(
    elf_path: Path, out_path: Path, addr2line: str = "arm-none-eabi-addr2line"
) -> None:
    out_path.write_text(generate_golden(elf_path, addr2line))
