"""armv8m and stm32 load as built-in plugins from `_core`'s init, through the plugin loader."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from elftools.elf.dynamic import DynamicSection
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import StringTableSection, SymbolTableSection
from popcorn import _core
from popcorn._native_digest import native_libraries

ENTRY = "popcorn_plugin_init"
BUILTINS = {"libpopcorn_armv8m.so", "libpopcorn_stm32.so"}


def defined_entries(path: Path) -> int:
    """How many dynamic symbols of @p path define the plugin entry point."""
    with path.open("rb") as stream:
        section = ELFFile(stream).get_section_by_name(".dynsym")
        assert isinstance(section, SymbolTableSection)
        return sum(
            1
            for symbol in section.iter_symbols()
            if symbol.name == ENTRY and symbol["st_shndx"] != "SHN_UNDEF"
        )


def plugin_symbols(path: Path) -> dict[str, tuple[str, str]]:
    """The defined `popcorn_plugin_*` dynamic symbols of @p path, as type and section name."""
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        section = elf.get_section_by_name(".dynsym")
        assert isinstance(section, SymbolTableSection)
        return {
            symbol.name: (
                symbol["st_info"]["type"],
                elf.get_section(symbol["st_shndx"]).name,
            )
            for symbol in section.iter_symbols()
            if symbol.name.startswith("popcorn_plugin_") and symbol["st_shndx"] != "SHN_UNDEF"
        }


def needed(path: Path) -> list[str]:
    """The `DT_NEEDED` entries of @p path."""
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        section = elf.get_section_by_name(".dynamic")
        strings = elf.get_section_by_name(".dynstr")
        assert isinstance(section, DynamicSection)
        assert isinstance(strings, StringTableSection)
        return [strings.get_string(tag["d_val"]) for tag in section.iter_tags("DT_NEEDED")]


def core_extension() -> Path:
    return Path(_core.__file__).resolve()


def static_archives() -> list[Path]:
    archives = sorted(core_extension().parent.parent.glob("libpopcorn_*_static.a"))
    if not archives:
        pytest.skip("static popcorn archives not built")
    return archives


def test_only_the_builtin_libraries_define_the_plugin_entry() -> None:
    counts = {path.name: defined_entries(path) for path in native_libraries()}
    assert counts.keys() >= BUILTINS
    assert {name: count for name, count in counts.items() if count} == dict.fromkeys(BUILTINS, 1)


def test_the_builtin_plugins_share_no_instrumented_data_object() -> None:
    libraries = {path.name: path for path in native_libraries()}
    for name in sorted(BUILTINS):
        assert plugin_symbols(libraries[name]) == {
            "popcorn_plugin_abi_tag": ("STT_OBJECT", ".popcorn_abi"),
            "popcorn_plugin_name": ("STT_FUNC", ".text"),
            "popcorn_plugin_init": ("STT_FUNC", ".text"),
        }, name


def test_no_static_archive_defines_the_plugin_entry() -> None:
    for archive in static_archives():
        listed = subprocess.run(
            ["nm", "--defined-only", str(archive)], capture_output=True, text=True, check=True
        )
        assert ENTRY not in listed.stdout.split(), archive.name


def test_core_does_not_link_armv8m_and_maps_it_at_import() -> None:
    assert "libpopcorn_armv8m.so" not in needed(core_extension())
    assert "libpopcorn_stm32.so" in needed(core_extension())
    probe = (
        "import sys\n"
        "def mapped():\n"
        "    with open('/proc/self/maps') as maps:\n"
        "        return 'libpopcorn_armv8m.so' in maps.read()\n"
        "before = mapped()\n"
        "import popcorn._core\n"
        "print(before, mapped())\n"
    )
    ran = subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True, check=True)
    assert ran.stdout.split() == ["False", "True"]


def test_the_core_extension_edge_waits_for_armv8m() -> None:
    ninja = core_extension().parents[1] / "build.ninja"
    if not ninja.is_file():
        pytest.skip(f"no {ninja}")
    edge = next(
        line
        for line in ninja.read_text().splitlines()
        if line.startswith(f"build popcorn/{core_extension().name}:")
    )
    assert "popcorn/libpopcorn_armv8m.so" in edge.split()


def edited_tag(tag: bytes) -> bytes:
    """@p tag with its compiler field's characters replaced, same length."""
    start = tag.index(b";gcc ") + len(b";gcc ")
    end = tag.index(b";", start)
    return tag[:start] + b"x" * (end - start) + tag[end:]


def copied_package(tmp_path: Path) -> Path:
    """A copy of the built popcorn package under @p tmp_path."""
    package = tmp_path / "popcorn"
    shutil.copytree(core_extension().parent, package, symlinks=True)
    return package


def imported_from(tmp_path: Path) -> subprocess.CompletedProcess[str]:
    """Import `popcorn._core` from the copy under @p tmp_path, exit 3 with the ImportError text."""
    probe = (
        "import sys\n"
        f"sys.path.insert(0, {str(tmp_path)!r})\n"
        "try:\n"
        "    import popcorn._core\n"
        "except ImportError as error:\n"
        "    print(type(error).__name__)\n"
        "    print(error)\n"
        "    raise SystemExit(3)\n"
    )
    return subprocess.run([sys.executable, "-I", "-c", probe], capture_output=True, text=True)


def test_a_missing_armv8m_fails_the_import_naming_the_file(tmp_path: Path) -> None:
    library = copied_package(tmp_path) / "libpopcorn_armv8m.so"
    library.unlink()
    ran = imported_from(tmp_path)
    assert ran.returncode == 3, ran.stderr
    assert str(library) in ran.stdout
    assert "path" in ran.stdout


def test_a_builtin_with_another_tag_fails_the_import_naming_both(tmp_path: Path) -> None:
    library = copied_package(tmp_path) / "libpopcorn_stm32.so"
    with library.open("rb") as stream:
        section = ELFFile(stream).get_section_by_name(".popcorn_abi")
        assert section is not None
        data: bytes = section.data()
    host = data.rstrip(b"\0")
    plugin = edited_tag(host)
    replacement = tmp_path / "tag.bin"
    replacement.write_bytes(plugin + data[len(host) :])
    subprocess.run(
        ["objcopy", f"--update-section=.popcorn_abi={replacement}", str(library)], check=True
    )
    dumped = subprocess.run(
        ["readelf", "-p", ".popcorn_abi", str(library)], capture_output=True, text=True, check=True
    )
    assert plugin.decode() in dumped.stdout

    ran = imported_from(tmp_path)
    assert ran.returncode == 3, ran.stderr
    assert "abi_tag" in ran.stdout
    assert host.decode() in ran.stdout
    assert plugin.decode() in ran.stdout
