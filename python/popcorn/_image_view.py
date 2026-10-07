"""Variables of the loaded ELF files as the firmware starts from them, `m.image.var("gains")`."""

from __future__ import annotations

import difflib
import functools
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from popcorn._dwarf_views import Var
from popcorn._errors import ImageError, SymbolError

if TYPE_CHECKING:
    from popcorn._image import Image
    from popcorn.image.dwarf import DebugInfo, Variable


@dataclass(frozen=True, slots=True)
class _Loadable:
    start: int
    data: bytes
    memory_size: int


@functools.lru_cache(maxsize=4)
def _loadables(resolved: str, mtime_ns: int, size: int) -> tuple[_Loadable, ...]:
    from elftools.common.exceptions import ELFError
    from elftools.elf.elffile import ELFFile

    try:
        with Path(resolved).open("rb") as stream:
            elf = ELFFile(stream)
            return tuple(
                _Loadable(int(s["p_vaddr"]), s.data()[: s["p_filesz"]], int(s["p_memsz"]))
                for s in elf.iter_segments()
                if s["p_type"] == "PT_LOAD"
            )
    except (OSError, ELFError) as error:
        raise ImageError(f"cannot read {resolved}: {error}") from error


class _ElfBytes:
    """The initial bytes of an ELF's PT_LOAD segments at their virtual addresses, zero past the
    file bytes up to the memory size. Read-only."""

    __slots__ = ("_loadables", "_path")

    def __init__(self, path: Path) -> None:
        resolved = path.resolve()
        try:
            stat = resolved.stat()
        except OSError as error:
            raise ImageError(f"cannot read {path}: {error}") from error
        self._path = path
        self._loadables = _loadables(str(resolved), stat.st_mtime_ns, stat.st_size)

    def read(self, address: int, size: int) -> bytes:
        end = address + size
        for index, loadable in enumerate(self._loadables):
            if loadable.start <= address and end <= loadable.start + loadable.memory_size:
                offset = address - loadable.start
                data = loadable.data[offset : offset + size]
                return data + bytes(size - len(data))
            if address < loadable.start + loadable.memory_size and loadable.start < end:
                raise ImageError(
                    f"{size} bytes at {address:#010x} cross the edge of loadable segment"
                    f" {index} of {self._path}",
                    segment=str(index),
                    address=address,
                )
        raise ImageError(
            f"{size} bytes at {address:#010x} lie in no loadable segment of {self._path}",
            address=address,
        )

    def write(self, address: int, data: bytes) -> None:
        raise TypeError("an image view is read-only, write the machine through m.mem or m.write")

    def write_masked(self, address: int, mask: bytes, data: bytes) -> None:
        self.write(address, data)


def debug_infos(images: Sequence[Image]) -> list[tuple[Image, DebugInfo]]:
    """The DWARF of each ELF image that has some, in load order."""
    from popcorn.image.dwarf import DebugInfoError, load_debug_info

    infos: list[tuple[Image, DebugInfo]] = []
    for image in images:
        if image.path is not None and image.path.suffix != ".hex" and image.segments:
            try:
                infos.append((image, load_debug_info(image.path)))
            except DebugInfoError:
                continue
    return infos


def find_variable(images: Sequence[Image], name: str) -> tuple[Image, DebugInfo, Variable]:
    """The module-scope variable `name`, the last loaded image first.

    Raises:
        SymbolError: No image declares the variable, with close matches.
    """
    known: list[str] = []
    for image, info in reversed(debug_infos(images)):
        variable = info.variables.get(name)
        if variable is not None:
            return image, info, variable
        known.extend(info.variables)
    close = difflib.get_close_matches(name, known, n=5)
    raise SymbolError(
        f"no variable {name} in the loaded images, close matches: {', '.join(close)}",
        name=name,
        close_matches=close,
    )


class ImageView:
    """The loaded ELF files as the firmware starts from them, `m.image`: variables typed from
    DWARF, read from the files, never from the machine.

    Readable parked or running, from any thread: it reads no machine state. A variable's value
    is its initializer: right for `const` data and lookup tables in flash, and for the value a
    `.data` variable starts with. Wrong for anything the firmware writes, `.bss` reads 0 and a
    variable set at runtime reads its initializer. Use `m.var` parked or `m.ref.var` running for
    those. Writes raise TypeError.
    """

    __slots__ = ("_images",)

    def __init__(self, images: Sequence[Image]) -> None:
        """View `images`. Use `Machine.image` instead."""
        self._images = tuple(images)

    def var(self, name: str) -> Var:
        """A module-scope variable of the loaded ELF images as their files hold it, the last
        loaded image first, as `Machine.var` looks it up. The file is read now: a file rebuilt
        since the load gives its new bytes.

        Raises:
            SymbolError: No loaded image declares the variable, with close matches.
            ImageError: The file cannot be read, or, from a read, the variable lies in no
                loadable segment of its file.
        """
        image, info, variable = find_variable(self._images, name)
        assert image.path is not None
        return Var(_ElfBytes(image.path), info, variable.type, variable.address, name)

    def __repr__(self) -> str:
        return f"ImageView(images={len(self._images)})"
