"""Constants of ELF files as their DWARF declares them, `popcorn.Elf(path).constants`."""

from __future__ import annotations

import collections
import difflib
import functools
import re
import threading
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._c_expr import BOOL, INT, LLONG, UINT, ULLONG, CType, CValue, Parsed, parse
from popcorn._errors import ImageError, MissingInput, SymbolError, absent

if TYPE_CHECKING:
    from popcorn.image.dwarf import DebugInfo, DwarfUnit, EnumType, TypeId

_CLASS_CAPACITY = 256


@dataclass(frozen=True, slots=True)
class _File:
    """The DWARF of one file, with its enumerators by name, the value and unit of each
    declaring enum type, and the DIEs of each enum tag and typedef name."""

    path: Path
    stamp: tuple[str, int, int]
    info: DebugInfo
    enumerators: dict[str, list[tuple[int, DwarfUnit | None]]]
    tags: dict[str, list[TypeId]]
    typedefs: dict[str, list[TypeId]]


@functools.lru_cache(maxsize=8)
def _indexed(resolved: str, mtime_ns: int, size: int) -> _File | None:
    from popcorn.image.dwarf import (
        DebugInfoError,
        EnumType,
        NoDebugInfo,
        Typedef,
        load_debug_info,
    )

    try:
        info = load_debug_info(Path(resolved))
    except NoDebugInfo:
        return None
    except DebugInfoError as error:
        raise ImageError(f"cannot read the DWARF of {resolved}: {error}") from error
    enumerators: dict[str, list[tuple[int, DwarfUnit | None]]] = {}
    tags: dict[str, list[TypeId]] = {}
    typedefs: dict[str, list[TypeId]] = {}
    for type_id, debug_type in info.by_id.items():
        if isinstance(debug_type, EnumType):
            unit = info.unit(type_id)
            for name, value in debug_type.values.items():
                enumerators.setdefault(name, []).append((value, unit))
            if debug_type.name is not None:
                tags.setdefault(debug_type.name, []).append(type_id)
        elif isinstance(debug_type, Typedef):
            typedefs.setdefault(debug_type.name, []).append(type_id)
    return _File(Path(resolved), (resolved, mtime_ns, size), info, enumerators, tags, typedefs)


class _Classes:
    """IntEnum classes by file stamp and enum DIE offset, the oldest dropped past capacity, so
    every lookup of one enum type in one file version gives the same class."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._classes: collections.OrderedDict[tuple[str, int, int, TypeId], type[IntEnum]] = (
            collections.OrderedDict()
        )

    def get(
        self, key: tuple[str, int, int, TypeId], name: str, members: Sequence[tuple[str, int]]
    ) -> type[IntEnum]:
        with self._lock:
            found = self._classes.get(key)
            if found is not None:
                self._classes.move_to_end(key)
                return found
            built = cast(
                type[IntEnum],
                IntEnum(_class_name(name), list(members), module="popcorn.firmware"),
            )
            self._classes[key] = built
            if len(self._classes) > _CLASS_CAPACITY:
                self._classes.popitem(last=False)
            return built


_CLASSES = _Classes()


def _class_name(name: str) -> str:
    return re.sub(r"\W", "_", name) or "_"


def _in_unit(unit: DwarfUnit | None, wanted: str) -> bool:
    return unit is not None and _named(unit.name, wanted)


def _unit_name(unit: DwarfUnit | None) -> str:
    return "<no unit>" if unit is None else unit.name


class Constants:
    """Constants of ELF files as their DWARF declares them, read from the files, never from a
    machine: usable at pytest collection, before any machine exists, and from any thread.

        channels = list(project.constants.enum("channel_t"))
        assert project.constants["BSP_DMA_CHANNEL_ID_MAX"] == 21

    Nothing is read until a lookup. Each lookup reads the files now, cached by path, mtime and
    size as `load_debug_info` caches. A name the last file declares hides the earlier files.
    Not a mapping: no `in`, `len` or iteration, a lookup of an absent name raises SymbolError,
    not KeyError.

    A name declared differently by several compile units of one file, a `static` enum in two
    `.c` files or a header built with different flags, raises SymbolError unless `unit=` picks
    one: its DW_AT_name, or a suffix of it on a `/` boundary, `"dma.c"`.

    Integer macros, `macro`, and their text, `macro_text`, come from `.debug_macro`, which GCC
    writes for firmware built with `-g3`: each unit's definitions as they stand at its end.

    GCC leaves out of the DWARF every type no declaration uses, `-feliminate-unused-debug-types`,
    its default. An enum the firmware never stores in a variable, a member or a parameter is
    absent, and so are its enumerators, unless the firmware is built with
    `-fno-eliminate-unused-debug-types`.
    """

    __slots__ = ("_files",)

    def __init__(self, *, _files: Iterable[tuple[str, Path]]) -> None:
        """Use `Elf.constants`, `MachineDescription.constants` or `m.image.constants`."""
        self._files = tuple(_files)

    def __getitem__(self, name: str) -> int:
        """The value of the enumerator or integer macro `name`,
        `constants["BSP_DMA_CHANNEL_ID_MAX"]`.

        An enumerator answers first. A macro of the same name that evaluates to another value
        raises. One that does not evaluate, text only or with a unit conflict, is ignored, so
        `#define CHANNEL_A CHANNEL_A` gives the enumerator. Without an enumerator, as
        `macro(name)`.

        Raises:
            SymbolError: The units disagree on the enumerator, as `enumerator`, or an
                enumerator and a macro of the name give different values, or neither exists,
                as `macro`.
            ValueError: No enumerator has the name and the macro is no integer expression, as
                `macro`.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        known: list[str] = []
        enumerator = self._enumerator(name, None, known)
        if enumerator is None:
            found = self._macro(name, None, known)
            if found is None:
                close = difflib.get_close_matches(name, known, n=5)
                raise SymbolError(
                    f"no enumerator or macro {name} in the files, close matches: "
                    + ", ".join(close),
                    name=name,
                    close_matches=close,
                )
            return _Evaluation(found).value(name, found.entry, found.units, ())
        try:
            macro = self.macro(name)
        except SymbolError, ValueError:
            return enumerator
        if macro != enumerator:
            raise SymbolError(
                f"the enumerator {name} is {enumerator} and the macro {name} is {macro}",
                name=name,
            )
        return enumerator

    def macro(self, name: str, *, unit: str | None = None) -> int:
        """The value of the object-like macro `name` as an integer constant expression, as it is
        defined at the end of the unit: `constants.macro("HAL_SPI_TRANSFER_LSB_FIRST")`.

        Integer literals with C suffixes, character literals, unary `+ - ~ !`, binary
        `* / % + - << >> < <= > >= == != & ^ | && ||`, `?:`, parentheses, casts to an integer
        type the file's DWARF names, and identifiers that are other object-like macros or
        enumerators. Arithmetic follows C on the type DWARF gives a cast, `int` otherwise,
        promoted as C promotes to 32-bit `int`, `long` and 64-bit `long long`, the target's
        widths. A macro inside another keeps its own type, as textual replacement does. An
        identifier resolves in the units that define `name` as found, and a name met inside its
        own expansion is an enumerator or an error, as the C preprocessor leaves it. Arithmetic
        C leaves undefined raises, even where GCC folds it with a warning: `(1 << 31)` raises,
        `(1u << 31)` reads. One lookup expands at most 100000 macros, nests at most 64 and
        evaluates at most 1000000 expression nodes.

        The firmware must be built with `-g3`, which keeps the macros in `.debug_macro`.

        Args:
            name: The macro.
            unit: A compile unit, as `enumerator` takes it. Needed only when units disagree.

        Raises:
            SymbolError: No unit defines the macro, with close matches and, per file, the
                count of `.debug_macro` imports whose section the linker discarded, or no file
                holds macros. Or the units give different texts and `unit` is None, every unit and
                its text in the message. Or `unit` matches no unit, or several, or that unit
                does not define it.
            ValueError: The text is not such an expression: a function-like macro, a string,
                `sizeof`, a float, an identifier that is neither macro nor enumerator,
                arithmetic C leaves undefined, more than 100000 macro expansions or a text
                nested too deep. The message holds the text, which `macro_text`
                returns.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        found = self._found_macro(name, unit)
        return _Evaluation(found).value(name, found.entry, found.units, ())

    def macro_text(self, name: str, *, unit: str | None = None) -> str:
        """The definition of the macro `name` as written after its name, for any macro:
        `"16"`, `"(x) ((x) * (x))"` for a function-like one, `""` for `#define X`.

        Args:
            name: The macro name.
            unit: The compilation unit to look in, any by default.

        Raises:
            SymbolError: As `macro`.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        return self._found_macro(name, unit).entry.text

    def _found_macro(self, name: str, unit: str | None) -> _FoundMacro:
        known: list[str] = []
        found = self._macro(name, unit, known)
        if found is not None:
            return found
        if not any(_macro_index(file) is not None for file in self._loaded()):
            raise SymbolError(
                f"no macro {name}: no file holds .debug_macro, build the firmware with -g3",
                name=name,
            )
        close = difflib.get_close_matches(name, known, n=5)
        discarded = "".join(
            f"; {file.path}: {index.discarded_imports()} .debug_macro imports name a section"
            " the linker discarded, the macros of their headers are missing"
            for file in self._loaded()
            if (index := _macro_index(file)) is not None and index.discarded_imports()
        )
        raise SymbolError(
            f"no macro {name} in the files, close matches: {', '.join(close)}{discarded}",
            name=name,
            close_matches=close,
        )

    def _macro(self, name: str, unit: str | None, known: list[str]) -> _FoundMacro | None:
        for file in self._loaded():
            index = _macro_index(file)
            if index is None:
                continue
            units = index.units()
            entries = index.lookup(name)
            if not entries:
                known.extend(index.names())
                continue
            if unit is not None:
                picked = _picked_macro_unit(file, units, unit, name)
                entries = [entry for entry in entries if picked in entry.units]
                if not entries:
                    raise SymbolError(
                        f"unit {units[picked]} of {file.path} defines no macro {name}", name=name
                    )
                return _FoundMacro(file, index, entries[0], frozenset((picked,)))
            if len(entries) > 1:
                raise SymbolError(
                    f"units of {file.path} define {name} differently, pick one with unit=: "
                    + ", ".join(
                        f"{units[at]}: {entry.text!r}" for entry in entries for at in entry.units
                    ),
                    name=name,
                    close_matches=sorted({units[at] for entry in entries for at in entry.units}),
                )
            return _FoundMacro(file, index, entries[0], frozenset(entries[0].units))
        return None

    def enumerator(self, name: str, *, unit: str | None = None) -> int:
        """The value of the enumerator `name`, in any enum type of the files, typedef'd or
        anonymous.

        Args:
            name: The enumerator.
            unit: A compile unit, its DW_AT_name or a path suffix of it, `"dma.c"`. Needed only
                when units disagree.

        Raises:
            SymbolError: No enumerator has the name, with close matches. Or the units that
                declare it give different values and `unit` is None, the units in
                `close_matches` and every unit's value in the message. Or `unit` matches no
                unit of the file declaring the name, or several, or that unit does not declare
                it.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        known: list[str] = []
        found = self._enumerator(name, unit, known)
        if found is None:
            raise self._absent("enumerator", name, known)
        return found

    def _enumerator(self, name: str, unit: str | None, known: list[str]) -> int | None:
        for file in self._loaded():
            entries = file.enumerators.get(name)
            if not entries:
                known.extend(file.enumerators)
                continue
            if unit is not None:
                picked = _picked_unit(file, unit, name)
                values = {value for value, at in entries if at == picked}
                if not values:
                    raise SymbolError(
                        f"unit {picked.name} of {file.path} declares no enumerator {name}",
                        name=name,
                        close_matches=[_unit_name(at) for _, at in entries],
                    )
                return values.pop()
            values = {value for value, _ in entries}
            if len(values) == 1:
                return values.pop()
            raise SymbolError(
                f"units of {file.path} give {name} different values, pick one with unit=: "
                + ", ".join(f"{_unit_name(at)} = {value}" for value, at in entries),
                name=name,
                close_matches=sorted({_unit_name(at) for _, at in entries}),
            )
        return None

    def enum(self, name: str, *, unit: str | None = None) -> type[IntEnum]:
        """An IntEnum of the enum type `name`, its members the DWARF enumerators in declaration
        order: a typedef, `"BSP_DMA_RequestFunction_t"`, or a tag, `"enum bsp_dma_request"` or
        `"bsp_dma_request"`. Typedefs and qualifiers are followed to the enum, so a typedef of
        an anonymous enum works. Equal values become aliases, as IntEnum makes them. A unit that
        only declares the enum, `enum state;`, is ignored when another unit defines it. Every
        lookup of one enum type of one file version returns the same class, through any entry
        point and any `unit=`, while fewer than 256 enum types were looked up since: past that
        the oldest class is dropped and built anew. Its `__module__` is `popcorn.firmware`,
        which cannot be imported: a member does not pickle.

        Args:
            name: The typedef or the tag.
            unit: A compile unit, as `enumerator` takes it. Needed only when units declare the
                name with different enumerators.

        Raises:
            SymbolError: No type has the name, with close matches. Or units declare it with
                different enumerator lists and `unit` is None, the units in `close_matches`. Or
                `unit` matches no unit of the file declaring the name, or several, or that unit
                does not declare it.
            TypeError: The type is not an enum, or is a declaration with no enumerators.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        tag = name.removeprefix("enum ")
        known: list[str] = []
        for file in self._loaded():
            info = file.info
            every, other = _enum_candidates(file, name, tag)
            if not every:
                if other is not None:
                    raise TypeError(f"{name} in {file.path} is a {other}, not an enum")
                known.extend(info.types)
                continue
            found = every
            if unit is not None:
                picked = _picked_unit(file, unit, name)
                found = [(type_id, enum) for type_id, enum in found if info.unit(type_id) == picked]
                if not found:
                    raise SymbolError(
                        f"unit {picked.name} of {file.path} declares no enum {name}",
                        name=name,
                    )
            found = [candidate for candidate in found if candidate[1].values] or found
            lists = {tuple(enum.values.items()) for _, enum in found}
            if len(lists) > 1:
                raise SymbolError(
                    f"units of {file.path} declare {name} with different enumerators, pick one"
                    " with unit=",
                    name=name,
                    close_matches=sorted({_unit_name(info.unit(type_id)) for type_id, _ in found}),
                )
            members = lists.pop()
            type_id, enum = min(
                (candidate for candidate in every if tuple(candidate[1].values.items()) == members),
                key=lambda candidate: candidate[0],
            )
            if not enum.values:
                raise TypeError(f"{name} in {file.path} is a declaration with no enumerators")
            resolved, mtime_ns, size = file.stamp
            return _CLASSES.get(
                (resolved, mtime_ns, size, type_id), name, list(enum.values.items())
            )
        raise self._absent("type", name, known)

    def _loaded(self) -> list[_File]:
        loaded: list[_File] = []
        for key, path in reversed(self._files):
            if absent(path):
                raise MissingInput(f"{key}: no file at {path}", path=path, key=key)
            resolved = path.resolve()
            try:
                stat = resolved.stat()
            except OSError as error:
                raise ImageError(f"cannot read {path}: {error}") from error
            file = _indexed(str(resolved), stat.st_mtime_ns, stat.st_size)
            if file is not None:
                loaded.append(file)
        return loaded

    def _absent(self, kind: str, name: str, known: list[str]) -> SymbolError:
        if not self._loaded():
            return SymbolError(f"no {kind} {name}: no file has DWARF", name=name)
        close = difflib.get_close_matches(name, known, n=5)
        return SymbolError(
            f"no {kind} {name} in the DWARF of the files, close matches: {', '.join(close)}",
            name=name,
            close_matches=close,
        )

    def __repr__(self) -> str:
        return f"Constants({', '.join(str(path) for _, path in self._files)})"


@functools.lru_cache(maxsize=4)
def _macros(resolved: str, mtime_ns: int, size: int) -> _core.MacroIndex | None:
    index = _core.read_macros(resolved)
    return index if index.units() else None


def _macro_index(file: _File) -> _core.MacroIndex | None:
    return _macros(*file.stamp)


def _picked_macro_unit(file: _File, units: Sequence[str], unit: str, name: str) -> int:
    matched = [at for at, candidate in enumerate(units) if _named(candidate, unit)]
    if len(matched) != 1:
        raise SymbolError(
            f"unit={unit} matches {len(matched)} units of {file.path}, not one",
            name=name,
            close_matches=[units[at] for at in matched] or list(units),
        )
    return matched[0]


@dataclass(frozen=True, slots=True)
class _FoundMacro:
    file: _File
    index: _core.MacroIndex
    entry: _core.MacroEntry
    units: frozenset[int]


_MAX_NESTING = 64
_MAX_EXPANSIONS = 100_000
_MAX_WORK = 1_000_000


class _Evaluation:
    """Evaluates macro texts of one file, each identifier in the units of the macro that
    holds it."""

    def __init__(self, found: _FoundMacro) -> None:
        self._file = found.file
        self._index = found.index
        self._names = found.index.units()
        self._expansions = 0
        self._work = 0
        self._entries: dict[str, list[_core.MacroEntry]] = {}
        self._parsed: dict[str, Parsed] = {}

    def _lookup(self, name: str) -> list[_core.MacroEntry]:
        found = self._entries.get(name)
        if found is None:
            found = self._entries[name] = self._index.lookup(name)
        return found

    def value(
        self, name: str, entry: _core.MacroEntry, units: frozenset[int], chain: tuple[str, ...]
    ) -> int:
        try:
            found = self._typed(name, entry, units, chain)
        except RecursionError:
            raise ValueError(f"{name} nests too deep, its text: {entry.text!r}") from None
        if self._expansions > _MAX_EXPANSIONS:
            raise ValueError(f"{name} expands more than {_MAX_EXPANSIONS} macros")
        if self._work > _MAX_WORK:
            raise ValueError(f"{name} evaluates more than {_MAX_WORK} expression nodes")
        return found.value

    def _typed(
        self, name: str, entry: _core.MacroEntry, units: frozenset[int], chain: tuple[str, ...]
    ) -> CValue:
        if entry.function_like:
            raise ValueError(f"{name} is a function-like macro, its text: {entry.text!r}")
        expanding = (*chain, name)
        try:
            parsed = self._parsed.get(entry.text)
            if parsed is None:
                parsed = self._parsed[entry.text] = parse(entry.text, self._type_name)
            self._work += parsed.size
            if self._work > _MAX_WORK:
                raise ValueError(f"evaluates more than {_MAX_WORK} expression nodes")
            return parsed.evaluate(
                lambda identifier: self._identifier(identifier, units, expanding)
            )
        except ValueError as error:
            raise ValueError(
                f"{name} is not an integer expression, its text: {entry.text!r}: {error}"
            ) from None

    def _identifier(self, name: str, units: frozenset[int], chain: tuple[str, ...]) -> CValue:
        if name not in chain:
            entries = [entry for entry in self._lookup(name) if units & set(entry.units)]
            if len({entry.text for entry in entries}) > 1:
                raise SymbolError(
                    f"units of {self._file.path} define {name} differently", name=name
                )
            if entries:
                if len(chain) >= _MAX_NESTING:
                    raise ValueError(f"more than {_MAX_NESTING} nested macros")
                self._expansions += 1
                if self._expansions > _MAX_EXPANSIONS:
                    raise ValueError(f"expands more than {_MAX_EXPANSIONS} macros")
                inner = units & frozenset(entries[0].units)
                return self._typed(name, entries[0], inner, chain)
        values = {
            value
            for value, unit in self._file.enumerators.get(name, ())
            if unit is not None and any(_named(unit.name, self._names[at]) for at in units)
        } or {value for value, _ in self._file.enumerators.get(name, ())}
        if len(values) > 1:
            raise SymbolError(f"units of {self._file.path} give {name} different values", name=name)
        if values:
            return _enumerator_value(values.pop())
        raise ValueError(
            f"{name} is neither macro nor enumerator, expanding {' -> '.join((*chain, name))}"
        )

    def _type_name(self, name: str) -> CType | None:
        from popcorn.image.dwarf import BaseType, DebugInfoError, Encoding, Typedef

        info = self._file.info
        type_id = info.types.get(name)
        if type_id is None or not isinstance(info.resolve(type_id), Typedef):
            return None
        found = _stripped_enum(info, type_id)
        if isinstance(found, tuple):
            enum: EnumType = found[1]
            signed = any(value < 0 for value in enum.values.values())
            return _sized(enum.size, signed, "")
        try:
            target = info.strip(type_id)
        except DebugInfoError:
            return None
        if not isinstance(target, BaseType) or target.encoding is Encoding.FLOAT:
            return None
        if target.encoding is Encoding.BOOLEAN:
            return BOOL
        return _sized(target.size, target.encoding is Encoding.SIGNED, target.name)


def _sized(size: int, signed: bool, name: str) -> CType | None:
    kinds = {
        1: (8, 1),
        2: (16, 1),
        4: (32, 3 if "long" in name and "long long" not in name else 2),
        8: (64, 4),
    }
    if size not in kinds:
        return None
    bits, rank = kinds[size]
    return CType(bits, signed, rank)


def _enumerator_value(value: int) -> CValue:
    for kind in (INT, UINT, LLONG, ULLONG):
        if kind.holds(value):
            return CValue(value, kind)
    raise ValueError(f"{value} fits no integer type")


def _named(candidate: str, wanted: str) -> bool:
    return candidate == wanted or candidate.endswith("/" + wanted)


def _picked_unit(file: _File, unit: str, name: str) -> DwarfUnit:
    matched = [candidate for candidate in file.info.units if _in_unit(candidate, unit)]
    if len(matched) != 1:
        raise SymbolError(
            f"unit={unit} matches {len(matched)} units of {file.path}, not one",
            name=name,
            close_matches=[candidate.name for candidate in matched or file.info.units],
        )
    return matched[0]


def _enum_candidates(
    file: _File, name: str, tag: str
) -> tuple[list[tuple[TypeId, EnumType]], str | None]:
    """The enum DIEs `name` reaches, as a tag or a typedef, and what a non-enum type of that
    name is."""
    from popcorn.image.dwarf import EnumType

    info = file.info
    found: dict[TypeId, EnumType] = {}
    other: str | None = None
    for type_id in file.tags.get(tag, ()):
        enum = info.resolve(type_id)
        if isinstance(enum, EnumType):
            found[type_id] = enum
    for type_id in file.typedefs.get(name, ()):
        target = _stripped_enum(info, type_id)
        if isinstance(target, tuple):
            found[target[0]] = target[1]
        else:
            other = target
    if found:
        return sorted(found.items()), None
    if other is None:
        keys = [name] if " " in name else [name, f"struct {name}", f"union {name}"]
        for key in keys:
            type_id = info.types.get(key)
            if type_id is not None:
                return [], type(info.resolve(type_id)).__name__
    return [], other


def _stripped_enum(info: DebugInfo, type_id: TypeId) -> tuple[TypeId, EnumType] | str:
    from popcorn.image.dwarf import EnumType, Qualified, Typedef

    seen = {type_id}
    current = info.resolve(type_id)
    while isinstance(current, Typedef | Qualified):
        target = current.target
        if target is None:
            return "typedef of void"
        if target in seen:
            return "typedef cycle"
        seen.add(target)
        type_id = target
        current = info.resolve(target)
    if isinstance(current, EnumType):
        return type_id, current
    return type(current).__name__
