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
    return unit is not None and (unit.name == wanted or unit.name.endswith("/" + wanted))


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

    GCC leaves out of the DWARF every type no declaration uses, `-feliminate-unused-debug-types`,
    its default. An enum the firmware never stores in a variable, a member or a parameter is
    absent, and so are its enumerators, unless the firmware is built with
    `-fno-eliminate-unused-debug-types`.
    """

    __slots__ = ("_files",)

    def __init__(self, *, _files: Iterable[tuple[str, Path]]) -> None:
        """Use `Elf.constants`, `Project.constants` or `m.image.constants`."""
        self._files = tuple(_files)

    def __getitem__(self, name: str) -> int:
        """The value of the enumerator `name`, `constants["BSP_DMA_CHANNEL_ID_MAX"]`, as
        `enumerator(name)`.

        Raises:
            SymbolError: As `enumerator`.
            MissingInput: A file does not exist.
            ImageError: A file is not a readable ELF or its DWARF is malformed.
        """
        return self.enumerator(name)

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
        raise self._absent("enumerator", name, known)

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
