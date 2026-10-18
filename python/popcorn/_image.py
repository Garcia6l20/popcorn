"""Firmware images: ELF, raw binary and Intel HEX files."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from popcorn import _core
from popcorn._enums import FloatAbi


@dataclass(frozen=True, slots=True)
class Location:
    """Program location: address, then symbol and source position when known."""

    pc: int
    symbol: str = ""
    offset: int = 0
    file: str = ""
    line: int = 0

    def __str__(self) -> str:
        where = f"{self.pc:#010x}"
        if self.symbol:
            where += f" {self.symbol}+{self.offset:#x}"
        if self.file:
            where += f" {self.file}:{self.line}"
        return where


@dataclass(frozen=True, slots=True)
class Segment:
    """Loadable part of an image."""

    name: str
    start: int
    file_size: int
    memory_size: int
    executable: bool
    writable: bool


@dataclass(frozen=True, slots=True)
class Symbol:
    """Symbol of an image."""

    name: str
    value: int
    size: int
    function: bool
    is_global: bool


def location_of(native: _core.Location) -> Location:
    """Python copy of a native location."""
    return Location(native.pc, native.symbol, native.offset, native.file, native.line)


def symbol_of(native: _core.SymbolInfo) -> Symbol:
    """Python copy of a native symbol."""
    return Symbol(native.name, native.value, native.size, native.function, native.is_global)


class Image:
    """A firmware image, immutable, loadable into any number of machines."""

    __slots__ = ("_elf", "_loaded", "_name", "_native", "_path")

    def __init__(
        self,
        native: _core.Image,
        path: Path | None = None,
        *,
        elf: bool = False,
        name: str | None = None,
        loaded: bool = True,
    ) -> None:
        """Wrap a native image. Use Image.elf, Image.bin or Image.hex instead."""
        self._native = native
        self._path = path
        self._elf = elf
        self._name = name
        self._loaded = loaded

    @property
    def native(self) -> _core.Image:
        """The native image the machine loads."""
        return self._native

    @classmethod
    def elf(
        cls, path: str | os.PathLike[str], *, name: str | None = None, load: bool = True
    ) -> Image:
        """Load an ELF32 file: segments, symbols, line table, entry and float ABI.

        Args:
            path: The file.
            name: Name that `image=` of the symbol lookups selects it by, None for none.
            load: False for a symbols-only image: a machine never writes its segments to
                memory and its snapshots hold no digest of it, while its symbols and DWARF
                feed `sym`, `fn_ptr`, `var`, `image`, the unwinder, coverage and trace as a
                loaded image's do, after every loaded image. For a part embedded as raw bytes
                in another image, such as a
                secure binary placed by `.incbin` in the Non-secure ELF.
        """
        return cls(_core.load_elf(os.fspath(path)), Path(path), elf=True, name=name, loaded=load)

    @classmethod
    def bin(cls, path: str | os.PathLike[str], at: int) -> Image:
        """Load a raw binary whose first byte goes at guest address `at`."""
        return cls(_core.load_bin(os.fspath(path), at), Path(path))

    @classmethod
    def hex(cls, path: str | os.PathLike[str]) -> Image:
        """Load an Intel HEX file."""
        return cls(_core.load_hex(os.fspath(path)), Path(path))

    @property
    def path(self) -> Path | None:
        """File the image was loaded from."""
        return self._path

    @property
    def name(self) -> str | None:
        """Name `image=` of the symbol lookups selects this image by, None for none."""
        return self._name

    @property
    def loaded(self) -> bool:
        """Whether a machine writes the image's segments to memory, False for a symbols-only
        image."""
        return self._loaded

    @property
    def is_elf(self) -> bool:
        """Whether the image was read from an ELF file, rather than a bin or HEX file."""
        return self._elf

    @property
    def digest(self) -> str:
        """SHA-256 of the loadable content, lower-case hex."""
        return self._native.digest()

    @property
    def entry(self) -> int | None:
        """Entry point, None when the file has none."""
        return self._native.entry()

    @property
    def abi(self) -> FloatAbi:
        """Floating-point calling convention."""
        return FloatAbi(self._native.abi().value)

    @property
    def segments(self) -> tuple[Segment, ...]:
        """Loadable segments, in address order."""
        return tuple(
            Segment(s.name, s.start, s.file_size, s.memory_size, s.executable, s.writable)
            for s in self._native.segments()
        )

    @property
    def symbols(self) -> tuple[Symbol, ...]:
        """Symbols, in address order."""
        return tuple(symbol_of(s) for s in self._native.symbols())

    def lookup(self, name: str) -> Symbol:
        """The symbol of that exact name, SymbolError with close matches when absent."""
        return symbol_of(self._native.lookup(name))

    def locate(self, address: int) -> Location:
        """Symbol, offset, file and line of an address in this image alone."""
        return location_of(self._native.locate(address))

    def __repr__(self) -> str:
        named = "" if self._name is None else f", name={self._name!r}"
        symbols_only = "" if self._loaded else ", load=False"
        return f"Image({self._path!s}, digest={self.digest[:12]}{named}{symbols_only})"
