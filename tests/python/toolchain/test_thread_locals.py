"""Each popcorn thread-local has one definition, in libpopcorn_core.so, which every library uses.

A link lock taken in usb or stm32 code must count in the variable core and _core read. An
inline or hidden thread-local would give each library its own copy, a definition of its own in
that library's symbol table.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from popcorn._native_digest import native_libraries

THREAD_LOCALS = (
    "_ZN7popcorn4core6detail15link_locks_heldE",
    "_ZN7popcorn4core6detail15host_locks_heldE",
    "_ZN7popcorn4core6detail12engine_ownerE",
    "_ZN7popcorn4core6detail14on_host_threadE",
)


def symbols(path: Path) -> dict[str, list[bool]]:
    """Whether each thread-local symbol entry of @p path is a definition, by name."""
    found: dict[str, list[bool]] = {}
    with path.open("rb") as stream:
        for section in ELFFile(stream).iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for symbol in section.iter_symbols():
                if symbol.name in THREAD_LOCALS:
                    found.setdefault(symbol.name, []).append(symbol["st_shndx"] != "SHN_UNDEF")
    return found


@pytest.mark.parametrize("name", THREAD_LOCALS)
def test_only_the_core_library_defines_a_thread_local(name: str) -> None:
    defining = sorted(path.name for path in native_libraries() if any(symbols(path).get(name, [])))
    assert defining == ["libpopcorn_core.so"]


def test_the_usb_and_stm32_libraries_use_the_core_link_lock_count() -> None:
    libraries = {path.name: path for path in native_libraries()}
    for name in ("libpopcorn_usb.so", "libpopcorn_stm32.so"):
        entries = symbols(libraries[name]).get(THREAD_LOCALS[0], [])
        assert entries
        assert not any(entries)
