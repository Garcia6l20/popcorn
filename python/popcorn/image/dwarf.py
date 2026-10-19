"""DWARF types, globals and functions read from an ELF's .debug_info through pyelftools.

`load_debug_info` walks every compile unit's whole DIE tree once: every type-kind DIE
wherever it occurs (including nested inside a struct or a subprogram), module-scope
variables with a single DW_OP_addr location, and module-scope subprograms. A function's locals
are read on first access to `Function.locals`, from the ELF reopened once per load; a location
list or range list is then read once per unit, whatever the number of DIEs sharing it. It never
runs on the `load_elf` path; callers use it lazily, on first access to a type or symbol view.

The input is an untrusted, possibly malformed or adversarial ELF: every DWARF attribute is
read defensively (form and type checked before use), and the whole walk is wrapped so any
parse failure becomes a single `DebugInfoError` naming the path, never a raw exception.
"""

from __future__ import annotations

import bisect
import functools
import os
import posixpath
import struct
import threading
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field, replace
from enum import Enum
from pathlib import Path, PurePosixPath
from typing import cast

from elftools.common.exceptions import DWARFError, ELFError
from elftools.construct import ConstructError
from elftools.dwarf.compileunit import CompileUnit
from elftools.dwarf.die import DIE, AttributeValue
from elftools.dwarf.dwarfinfo import DWARFInfo
from elftools.dwarf.locationlists import BaseAddressEntry as LocationBaseEntry
from elftools.dwarf.locationlists import LocationEntry, LocationExpr, LocationParser
from elftools.dwarf.ranges import BaseAddressEntry
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

type TypeId = int


class DebugInfoError(ValueError):
    """An ELF could not be opened, has no DWARF info, or is too malformed to walk."""


class NoDebugInfo(DebugInfoError):
    """The ELF has no .debug_info compile unit."""


class Encoding(Enum):
    """DW_AT_encoding of a base type, collapsed to the categories callers need."""

    SIGNED = "signed"
    UNSIGNED = "unsigned"
    FLOAT = "float"
    BOOLEAN = "boolean"
    CHAR = "char"


class Qualifier(Enum):
    """Which DW_TAG_*_type a Qualified wraps."""

    CONST = "const"
    VOLATILE = "volatile"
    RESTRICT = "restrict"
    ATOMIC = "atomic"


@dataclass(frozen=True, slots=True)
class Decl:
    """The source file and line a DIE was declared at, normalized like step 05's line table."""

    file: str
    line: int


@dataclass(frozen=True, slots=True)
class BaseType:
    """A DW_TAG_base_type: a named, sized, primitive value."""

    name: str
    size: int
    encoding: Encoding


@dataclass(frozen=True, slots=True)
class PointerType:
    """A DW_TAG_pointer_type; `target` is None for `void*`."""

    target: TypeId | None
    size: int


@dataclass(frozen=True, slots=True)
class Member:
    """One field of a StructType: byte offset, plus bitfield position when applicable."""

    name: str
    offset: int
    type: TypeId
    bit_size: int | None
    bit_offset: int | None


@dataclass(frozen=True, slots=True)
class StructType:
    """A DW_TAG_structure_type or DW_TAG_union_type."""

    name: str | None
    size: int
    members: tuple[Member, ...]
    union: bool
    decl: Decl | None


@dataclass(frozen=True, slots=True)
class ArrayType:
    """A DW_TAG_array_type; a count is None for an unbounded or flexible dimension."""

    element: TypeId
    counts: tuple[int | None, ...]


@dataclass(frozen=True, slots=True)
class EnumType:
    """A DW_TAG_enumeration_type."""

    name: str | None
    size: int
    values: Mapping[str, int]


@dataclass(frozen=True, slots=True)
class Typedef:
    """A DW_TAG_typedef; `target` is None for a typedef of void."""

    name: str
    target: TypeId | None


@dataclass(frozen=True, slots=True)
class Qualified:
    """A const/volatile/restrict/atomic wrapper around another type."""

    qualifier: Qualifier
    target: TypeId | None


@dataclass(frozen=True, slots=True)
class FunctionType:
    """A DW_TAG_subroutine_type: the pointee shape of a function pointer."""

    returns: TypeId | None
    params: tuple[TypeId, ...]
    variadic: bool


type DebugType = (
    BaseType | PointerType | StructType | ArrayType | EnumType | Typedef | Qualified | FunctionType
)


@dataclass(frozen=True, slots=True)
class Variable:
    """A module-scope DW_TAG_variable with a single DW_OP_addr location."""

    name: str
    address: int
    type: TypeId
    decl: Decl | None


@dataclass(frozen=True, slots=True)
class Parameter:
    """One formal parameter of a Function."""

    name: str | None
    type: TypeId


@dataclass(frozen=True, slots=True)
class LocationRange:
    """One entry of a location list: `expr` locates the value while `low <= pc < high`."""

    low: int
    high: int
    expr: bytes


@dataclass(frozen=True, slots=True)
class Local:
    """A DW_TAG_variable or DW_TAG_formal_parameter in a function body, its own or one of a
    function inlined into it.

    `location` is a DWARF expression valid wherever the local is in scope, a location list, or
    None when the compiler kept no location, a local folded to a DW_AT_const_value included.
    A location list leaves out DW_LLE_default_location entries. `scopes` are the pc ranges of
    the innermost lexical block or inlined subroutine holding the local, empty when that is
    the function. `inlined` names the inlined function the local belongs to, None for the
    function's own.
    """

    name: str
    type: TypeId
    parameter: bool
    location: bytes | tuple[LocationRange, ...] | None
    scopes: tuple[tuple[int, int], ...]
    inlined: str | None
    decl: Decl | None


@dataclass(frozen=True, slots=True)
class Function:
    """A module-scope DW_TAG_subprogram; `address` is None for an inline-only function.

    `frame_base` is the DW_AT_frame_base expression of the DIE with code, None for a function
    without one or with a location list as frame base, which is never parsed.
    """

    name: str
    address: int | None
    returns: TypeId | None
    params: tuple[Parameter, ...]
    variadic: bool
    decl: Decl | None
    frame_base: bytes | None = None
    _locals: _LazyLocals | None = field(default=None, compare=False, repr=False)

    @property
    def locals(self) -> tuple[Local, ...]:
        """The locals of the body of the DIE with code, empty for a function without one.

        A local with no name along its origin chain and a block-scope `extern` declaration
        are left out, so the parameters here need not line up with `params`.

        Read on first access from the ELF `load_debug_info` read, reopened once per load. Its
        DWARF and every DIE read stay in memory while any Function of that load lives, about 15
        times the ELF size once every function's locals were read: on a 7.9 MB LTO ELF, 12 MB
        after the load, 18 MB after one function's locals, 119 MB after all of them.
        Equality, hashing, repr and pickling never read them.

        Raises:
            DebugInfoError: The ELF changed or vanished since it was loaded, or its DWARF is
                malformed.
        """
        return () if self._locals is None else self._locals.get()


@dataclass(frozen=True, slots=True)
class DwarfUnit:
    """A compile unit: its DW_AT_name and the .debug_info offsets of its DIEs, `start` to
    `end` exclusive."""

    name: str
    start: int
    end: int


@dataclass(frozen=True, slots=True)
class DebugInfo:
    """Every type, module-scope variable and function read from one ELF's .debug_info, and its
    compile units in offset order."""

    by_id: Mapping[TypeId, DebugType]
    types: Mapping[str, TypeId]
    variables: Mapping[str, Variable]
    functions: Mapping[str, Function]
    units: tuple[DwarfUnit, ...] = ()

    def unit(self, type_id: TypeId) -> DwarfUnit | None:
        """The compile unit holding the DIE at offset `type_id`, None when none does."""
        index = bisect.bisect_right(self.units, type_id, key=lambda unit: unit.start) - 1
        if index < 0 or type_id >= self.units[index].end:
            return None
        return self.units[index]

    def type(self, name: str) -> DebugType:
        type_id = self.types.get(name)
        if type_id is None:
            raise KeyError(name)
        return self.resolve(type_id)

    def resolve(self, type_id: TypeId) -> DebugType:
        return self.by_id[type_id]

    def strip(self, type_id: TypeId) -> DebugType:
        resolved = self.resolve(type_id)
        visited = {type_id}
        while isinstance(resolved, Typedef | Qualified):
            if resolved.target is None:
                raise DebugInfoError("strip: void has no underlying type")
            if resolved.target in visited:
                raise DebugInfoError(f"strip: cycle back to type {resolved.target}")
            visited.add(resolved.target)
            resolved = self.resolve(resolved.target)
        return resolved


_REF_FORMS = frozenset(
    (
        "DW_FORM_ref1",
        "DW_FORM_ref2",
        "DW_FORM_ref4",
        "DW_FORM_ref8",
        "DW_FORM_ref",
        "DW_FORM_ref_udata",
        "DW_FORM_ref_addr",
        "DW_FORM_ref_sig8",
        "DW_FORM_ref_sup4",
        "DW_FORM_ref_sup8",
        "DW_FORM_GNU_ref_alt",
    )
)

_ENCODINGS: dict[int, Encoding] = {
    0x02: Encoding.BOOLEAN,
    0x04: Encoding.FLOAT,
    0x05: Encoding.SIGNED,
    0x06: Encoding.SIGNED,
    0x07: Encoding.UNSIGNED,
    0x08: Encoding.UNSIGNED,
    0x0D: Encoding.SIGNED,
    0x0E: Encoding.UNSIGNED,
    0x10: Encoding.CHAR,
    0x11: Encoding.CHAR,
    0x12: Encoding.CHAR,
}

_QUALIFIERS: dict[str, Qualifier] = {
    "DW_TAG_const_type": Qualifier.CONST,
    "DW_TAG_volatile_type": Qualifier.VOLATILE,
    "DW_TAG_restrict_type": Qualifier.RESTRICT,
    "DW_TAG_atomic_type": Qualifier.ATOMIC,
}

_TYPE_TAGS = frozenset(
    (
        "DW_TAG_base_type",
        "DW_TAG_pointer_type",
        "DW_TAG_structure_type",
        "DW_TAG_union_type",
        "DW_TAG_array_type",
        "DW_TAG_enumeration_type",
        "DW_TAG_typedef",
        "DW_TAG_const_type",
        "DW_TAG_volatile_type",
        "DW_TAG_restrict_type",
        "DW_TAG_atomic_type",
        "DW_TAG_subroutine_type",
        "DW_TAG_reference_type",
        "DW_TAG_rvalue_reference_type",
        "DW_TAG_unspecified_type",
    )
)

_TAG_PREFIXES: dict[str, str] = {
    "DW_TAG_structure_type": "struct ",
    "DW_TAG_union_type": "union ",
    "DW_TAG_enumeration_type": "enum ",
}


def _type_key(tag: str, name: str) -> str:
    return f"{_TAG_PREFIXES.get(tag, '')}{name}"


def _attr_str(die: DIE, name: str) -> str | None:
    attr = die.attributes.get(name)
    if attr is None:
        return None
    value = attr.value
    if not isinstance(value, bytes):
        return None
    return value.decode("utf-8", "surrogateescape")


def _attr_int(die: DIE, name: str) -> int | None:
    attr = die.attributes.get(name)
    if attr is None or attr.form in _REF_FORMS:
        return None
    value = attr.value
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def _type_ref(die: DIE, name: str = "DW_AT_type") -> TypeId | None:
    if name not in die.attributes:
        return None
    return die.get_DIE_from_attribute(name).offset


def _read_uleb128(data: bytes, pos: int) -> int | None:
    result = 0
    shift = 0
    while pos < len(data):
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result
        shift += 7
        if shift > 64:
            return None
    return None


def _data_member_location(die: DIE) -> int:
    attr = die.attributes.get("DW_AT_data_member_location")
    if attr is None:
        return 0
    value = attr.value
    if attr.form not in _REF_FORMS and isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, list):
        block = bytes(cast("list[int]", value))
        if len(block) >= 1 and block[0] == 0x23:
            decoded = _read_uleb128(block, 1)
            if decoded is not None:
                return decoded
    return 0


def _safe_children(die: DIE) -> Iterator[DIE]:
    last_offset = -1
    limit = max(die.cu.size, 1)
    for count, child in enumerate(die.iter_children()):
        if child.offset <= last_offset or count >= limit:
            return
        last_offset = child.offset
        yield child


def _normalize(path: str) -> str:
    return posixpath.normpath(path)


def _join(comp_dir: str, directory: str, name: str) -> str:
    name_path = PurePosixPath(name)
    if name_path.is_absolute():
        return _normalize(str(name_path))
    dir_path = PurePosixPath(directory)
    if dir_path.is_absolute():
        return _normalize(str(dir_path / name))
    return _normalize(str(PurePosixPath(comp_dir) / directory / name))


def _decl_file_path(cu: CompileUnit, comp_dir: str, file_idx: int | None) -> str | None:
    if file_idx is None:
        return None
    dwarf_info = cu.dwarfinfo
    line_program = dwarf_info.line_program_for_CU(cu)
    if line_program is None:
        return None
    header = line_program.header
    version = cast(int, header["version"])
    file_entries = header["file_entry"]
    include_dirs = header["include_directory"]

    if version < 5:
        idx = file_idx - 1
        if idx < 0 or idx >= len(file_entries):
            return None
        entry = file_entries[idx]
        name = cast(bytes, entry.name).decode("utf-8", "surrogateescape")
        dir_index = cast(int, entry.dir_index)
        if dir_index == 0:
            return _join(comp_dir, "", name)
        if dir_index > len(include_dirs):
            return None
        directory = cast(bytes, include_dirs[dir_index - 1]).decode("utf-8", "surrogateescape")
        return _join(comp_dir, directory, name)

    if file_idx >= len(file_entries):
        return None
    entry = file_entries[file_idx]
    name = cast(bytes, entry.name).decode("utf-8", "surrogateescape")
    dir_index = cast(int, entry.dir_index)
    if dir_index == 0:
        return _join(comp_dir, "", name)
    if dir_index >= len(include_dirs):
        return None
    directory = cast(bytes, include_dirs[dir_index]).decode("utf-8", "surrogateescape")
    return _join(comp_dir, directory, name)


def _decl(cu: CompileUnit, comp_dir: str, die: DIE) -> Decl | None:
    file_idx = _attr_int(die, "DW_AT_decl_file")
    line = _attr_int(die, "DW_AT_decl_line")
    if file_idx is None or line is None:
        return None
    path = _decl_file_path(cu, comp_dir, file_idx)
    if path is None:
        return None
    return Decl(file=path, line=line)


def _build_base_type(die: DIE) -> BaseType:
    encoding_code = _attr_int(die, "DW_AT_encoding")
    encoding = (
        Encoding.SIGNED if encoding_code is None else _ENCODINGS.get(encoding_code, Encoding.SIGNED)
    )
    return BaseType(
        name=_attr_str(die, "DW_AT_name") or "",
        size=_attr_int(die, "DW_AT_byte_size") or 0,
        encoding=encoding,
    )


def _build_pointer_type(die: DIE) -> PointerType:
    return PointerType(target=_type_ref(die), size=_attr_int(die, "DW_AT_byte_size") or 4)


def _member_bit_layout(die: DIE, storage_size: int) -> tuple[int, int | None, int | None]:
    bit_size = _attr_int(die, "DW_AT_bit_size")
    if bit_size is None:
        return _data_member_location(die), None, None

    data_bit_offset = _attr_int(die, "DW_AT_data_bit_offset")
    if data_bit_offset is not None:
        return data_bit_offset // 8, data_bit_offset % 8, bit_size

    legacy_bit_offset = _attr_int(die, "DW_AT_bit_offset") or 0
    byte_size = _attr_int(die, "DW_AT_byte_size") or storage_size
    lsb_offset = byte_size * 8 - legacy_bit_offset - bit_size
    return _data_member_location(die) + lsb_offset // 8, lsb_offset % 8, bit_size


def _build_struct_type(cu: CompileUnit, comp_dir: str, die: DIE, union: bool) -> StructType:
    size = _attr_int(die, "DW_AT_byte_size") or 0
    members: list[Member] = []
    for child in _safe_children(die):
        if child.tag != "DW_TAG_member":
            continue
        name = _attr_str(child, "DW_AT_name") or ""
        type_id = _type_ref(child)
        if type_id is None:
            continue
        offset, bit_offset, bit_size = _member_bit_layout(child, size)
        members.append(
            Member(name=name, offset=offset, type=type_id, bit_size=bit_size, bit_offset=bit_offset)
        )
    return StructType(
        name=_attr_str(die, "DW_AT_name"),
        size=size,
        members=tuple(members),
        union=union,
        decl=_decl(cu, comp_dir, die),
    )


def _build_array_type(die: DIE) -> ArrayType:
    element = _type_ref(die)
    if element is None:
        raise DebugInfoError("array type with no element type")
    counts: list[int | None] = []
    for child in _safe_children(die):
        if child.tag != "DW_TAG_subrange_type":
            continue
        count = _attr_int(child, "DW_AT_count")
        if count is None:
            upper_bound = _attr_int(child, "DW_AT_upper_bound")
            count = upper_bound + 1 if upper_bound is not None else None
        counts.append(count)
    return ArrayType(element=element, counts=tuple(counts))


_SIGNED_ENCODINGS = frozenset((0x05, 0x06))
_UNSIGNED_DATA_FORMS = frozenset(
    ("DW_FORM_data1", "DW_FORM_data2", "DW_FORM_data4", "DW_FORM_data8")
)


def _signed_underlying(die: DIE) -> bool:
    """Whether the enum's DW_AT_type strips to a signed base type. False without DW_AT_type."""
    seen: set[int] = set()
    while "DW_AT_type" in die.attributes and die.offset not in seen:
        seen.add(die.offset)
        die = die.get_DIE_from_attribute("DW_AT_type")
        if die.tag == "DW_TAG_base_type":
            return _attr_int(die, "DW_AT_encoding") in _SIGNED_ENCODINGS
        if die.tag != "DW_TAG_typedef" and die.tag not in _QUALIFIERS:
            return False
    return False


def _build_enum_type(die: DIE) -> EnumType:
    size = _attr_int(die, "DW_AT_byte_size") or 0
    signed = 0 < size <= 8 and _signed_underlying(die)
    values: dict[str, int] = {}
    for child in _safe_children(die):
        if child.tag != "DW_TAG_enumerator":
            continue
        name = _attr_str(child, "DW_AT_name")
        value = _attr_int(child, "DW_AT_const_value")
        if name is None or value is None:
            continue
        form = child.attributes["DW_AT_const_value"].form
        if signed and form in _UNSIGNED_DATA_FORMS and value >= 1 << (8 * size - 1):
            value -= 1 << (8 * size)
        values[name] = value
    return EnumType(name=_attr_str(die, "DW_AT_name"), size=size, values=values)


def _build_typedef(die: DIE) -> Typedef:
    return Typedef(name=_attr_str(die, "DW_AT_name") or "", target=_type_ref(die))


def _build_qualified(die: DIE) -> Qualified:
    tag = die.tag
    if not isinstance(tag, str) or tag not in _QUALIFIERS:
        raise DebugInfoError(f"unhandled type tag {tag!r}")
    return Qualified(qualifier=_QUALIFIERS[tag], target=_type_ref(die))


def _build_subroutine_type(die: DIE) -> FunctionType:
    params: list[TypeId] = []
    variadic = False
    for child in _safe_children(die):
        if child.tag == "DW_TAG_formal_parameter":
            type_id = _type_ref(child)
            if type_id is not None:
                params.append(type_id)
        elif child.tag == "DW_TAG_unspecified_parameters":
            variadic = True
    return FunctionType(returns=_type_ref(die), params=tuple(params), variadic=variadic)


def _build_type(cu: CompileUnit, comp_dir: str, die: DIE) -> DebugType:
    if die.tag == "DW_TAG_base_type":
        return _build_base_type(die)
    if die.tag == "DW_TAG_pointer_type":
        return _build_pointer_type(die)
    if die.tag == "DW_TAG_structure_type":
        return _build_struct_type(cu, comp_dir, die, union=False)
    if die.tag == "DW_TAG_union_type":
        return _build_struct_type(cu, comp_dir, die, union=True)
    if die.tag == "DW_TAG_array_type":
        return _build_array_type(die)
    if die.tag == "DW_TAG_enumeration_type":
        return _build_enum_type(die)
    if die.tag == "DW_TAG_typedef":
        return _build_typedef(die)
    if die.tag == "DW_TAG_subroutine_type":
        return _build_subroutine_type(die)
    if die.tag in ("DW_TAG_reference_type", "DW_TAG_rvalue_reference_type"):
        return _build_pointer_type(die)
    if die.tag == "DW_TAG_unspecified_type":
        return BaseType(name=_attr_str(die, "DW_AT_name") or "", size=0, encoding=Encoding.SIGNED)
    return _build_qualified(die)


def _record_type(
    cu: CompileUnit,
    comp_dir: str,
    die: DIE,
    by_id: dict[TypeId, DebugType],
    types: dict[str, TypeId],
    type_is_decl: dict[str, bool],
) -> None:
    debug_type = _build_type(cu, comp_dir, die)
    by_id[die.offset] = debug_type
    name = _attr_str(die, "DW_AT_name")
    if name is None:
        return
    key = _type_key(cast(str, die.tag), name)
    is_decl = die.attributes.get("DW_AT_declaration") is not None
    if key not in types or (type_is_decl.get(key, False) and not is_decl):
        types[key] = die.offset
        type_is_decl[key] = is_decl


def _addr_from_location(attr: AttributeValue) -> int | None:
    if attr.form not in ("DW_FORM_exprloc", "DW_FORM_block1", "DW_FORM_block2", "DW_FORM_block4"):
        return None
    value = attr.value
    if not isinstance(value, list):
        return None
    raw = bytes(cast("list[int]", value))
    if len(raw) != 5 or raw[0] != 0x03:
        return None
    return struct.unpack_from("<I", raw, 1)[0]


def _origin_chain(die: DIE) -> list[DIE]:
    chain = [die]
    for attribute in ("DW_AT_abstract_origin", "DW_AT_specification"):
        if attribute in chain[-1].attributes:
            chain.append(chain[-1].get_DIE_from_attribute(attribute))
    return chain


def _chain_decl(chain: list[DIE]) -> Decl | None:
    line = next(
        (found for link in chain if (found := _attr_int(link, "DW_AT_decl_line")) is not None),
        None,
    )
    file_link = next((link for link in chain if "DW_AT_decl_file" in link.attributes), None)
    if not line or file_link is None:
        return None
    cu = file_link.cu
    if not isinstance(cu, CompileUnit):
        return None
    comp_dir = _attr_str(cu.get_top_DIE(), "DW_AT_comp_dir") or ""
    path = _decl_file_path(cu, comp_dir, _attr_int(file_link, "DW_AT_decl_file"))
    if path is None:
        return None
    return Decl(file=path, line=line)


def _build_variable(die: DIE) -> tuple[str, Variable, bool] | None:
    location = die.attributes.get("DW_AT_location")
    if location is None:
        return None
    address = _addr_from_location(location)
    if address is None:
        return None

    chain = _origin_chain(die)
    name = _attr_str(chain[-1], "DW_AT_name")
    type_id = _type_ref(chain[-1])
    if name is None or type_id is None:
        return None
    external = any(link.attributes.get("DW_AT_external") is not None for link in chain)
    variable = Variable(name=name, address=address, type=type_id, decl=_chain_decl(chain))
    return name, variable, external


def _resolve_variables(candidates: dict[str, list[tuple[Variable, bool]]]) -> dict[str, Variable]:
    resolved: dict[str, Variable] = {}
    for name, items in candidates.items():
        if len(items) == 1:
            resolved[name] = items[0][0]
            continue
        externals = [variable for variable, external in items if external]
        if len(externals) == 1:
            resolved[name] = externals[0]
    return resolved


def _merge_function(
    functions: dict[str, Function],
    function_is_decl: dict[str, bool],
    function: Function,
    is_decl: bool,
) -> None:
    existing = functions.get(function.name)
    rank = (function.address is not None, not is_decl)
    if (
        existing is not None
        and (existing.address is not None, not function_is_decl[function.name]) >= rank
    ):
        return
    functions[function.name] = function
    function_is_decl[function.name] = is_decl


def _range_pairs(cu: CompileUnit, die: DIE) -> list[tuple[int, int]]:
    attr = die.attributes.get("DW_AT_ranges")
    if attr is None or attr.form in _REF_FORMS:
        return []
    offset = attr.value
    if isinstance(offset, bool) or not isinstance(offset, int):
        return []
    range_lists = cu.dwarfinfo.range_lists()
    if range_lists is None:
        return []
    entries = range_lists.get_range_list_at_offset(offset, cu=cu)
    base = _attr_int(cu.get_top_DIE(), "DW_AT_low_pc") or 0
    pairs: list[tuple[int, int]] = []
    for entry in entries:
        if isinstance(entry, BaseAddressEntry):
            base = entry.base_address
        elif entry.is_absolute:
            pairs.append((entry.begin_offset, entry.end_offset))
        else:
            pairs.append((base + entry.begin_offset, base + entry.end_offset))
    return pairs


def address_from_ranges(cu: CompileUnit, die: DIE) -> int | None:
    starts = [low for low, _ in _range_pairs(cu, die)]
    return min(starts) if starts else None


type _Location = bytes | tuple[LocationRange, ...] | None
type _Ranges = tuple[tuple[int, int], ...]


@dataclass(slots=True)
class _Unit:
    """One compile unit's location and range lists, each parsed once whatever the number of
    DIEs that share it."""

    cu: CompileUnit
    parser: LocationParser
    location_lists: dict[tuple[str, int], _Location] = field(
        default_factory=dict[tuple[str, int], _Location]
    )
    range_lists: dict[int, _Ranges] = field(default_factory=dict[int, _Ranges])


def _pc_ranges(unit: _Unit, die: DIE) -> _Ranges:
    low = _attr_int(die, "DW_AT_low_pc")
    high_attr = die.attributes.get("DW_AT_high_pc")
    if low is not None and high_attr is not None:
        high = high_attr.value
        if isinstance(high, bool) or not isinstance(high, int):
            return ()
        end = high if high_attr.form.startswith("DW_FORM_addr") else low + high
        return ((low, end),) if end > low else ()
    attr = die.attributes.get("DW_AT_ranges")
    if attr is None or isinstance(attr.value, bool) or not isinstance(attr.value, int):
        return ()
    cached = unit.range_lists.get(attr.value)
    if cached is None:
        cached = tuple(pair for pair in _range_pairs(unit.cu, die) if pair[1] > pair[0])
        unit.range_lists[attr.value] = cached
    return cached


def _expr(value: object) -> bytes | None:
    if not isinstance(value, list):
        return None
    return bytes(cast("list[int]", value))


def _location_list(unit: _Unit, entries: Iterable[object]) -> _Location:
    base = _attr_int(unit.cu.get_top_DIE(), "DW_AT_low_pc") or 0
    ranges: list[LocationRange] = []
    for entry in entries:
        if isinstance(entry, LocationBaseEntry):
            base = entry.base_address
        elif isinstance(entry, LocationEntry):
            expr = _expr(entry.loc_expr)
            low = entry.begin_offset if entry.is_absolute else base + entry.begin_offset
            high = entry.end_offset if entry.is_absolute else base + entry.end_offset
            if expr is not None and high > low:
                ranges.append(LocationRange(low=low, high=high, expr=expr))
    return tuple(ranges) or None


def _location(unit: _Unit, die: DIE, name: str) -> _Location:
    attr = die.attributes.get(name)
    version = cast(int, unit.cu.header.version)
    if attr is None or not LocationParser.attribute_has_location(attr, version):
        return None
    value = attr.value
    key = (attr.form, value) if isinstance(value, int) and not isinstance(value, bool) else None
    if key is not None and key in unit.location_lists:
        return unit.location_lists[key]
    parsed = unit.parser.parse_from_attribute(attr, version, die)
    if isinstance(parsed, LocationExpr):
        return _expr(parsed.loc_expr)
    location = _location_list(unit, parsed)
    if key is not None:
        unit.location_lists[key] = location
    return location


def _build_local(unit: _Unit, die: DIE, scopes: _Ranges, inlined: str | None) -> Local | None:
    chain = _origin_chain(die)
    name = next((found for link in chain if (found := _attr_str(link, "DW_AT_name"))), None)
    type_id = next((ref for link in chain if (ref := _type_ref(link)) is not None), None)
    if name is None or type_id is None:
        return None
    return Local(
        name=name,
        type=type_id,
        parameter=die.tag == "DW_TAG_formal_parameter",
        location=_location(unit, die, "DW_AT_location"),
        scopes=scopes,
        inlined=inlined,
        decl=_chain_decl(chain),
    )


def _frame_base(unit: _Unit, die: DIE) -> bytes | None:
    attr = die.attributes.get("DW_AT_frame_base")
    if attr is None:
        return None
    version = cast(int, unit.cu.header.version)
    if attr.form == "DW_FORM_exprloc" or (version < 4 and attr.form.startswith("DW_FORM_block")):
        return _expr(attr.value)
    return None


def _collect_locals(unit: _Unit, die: DIE) -> tuple[Local, ...]:
    found: list[Local] = []
    stack: list[tuple[DIE, _Ranges, str | None]] = [
        (child, (), None) for child in reversed(list(_safe_children(die)))
    ]
    seen: set[int] = set()
    while stack:
        child, scopes, inlined = stack.pop()
        if child.offset in seen:
            continue
        seen.add(child.offset)
        if child.attributes.get("DW_AT_declaration") is not None:
            continue
        if child.tag in ("DW_TAG_variable", "DW_TAG_formal_parameter"):
            local = _build_local(unit, child, scopes, inlined)
            if local is not None:
                found.append(local)
            continue
        if child.tag == "DW_TAG_lexical_block":
            inner = _pc_ranges(unit, child) or scopes
        elif child.tag == "DW_TAG_inlined_subroutine":
            inner = _pc_ranges(unit, child) or scopes
            inlined = _attr_str(_origin_chain(child)[-1], "DW_AT_name")
        else:
            continue
        grandchildren = reversed(list(_safe_children(child)))
        stack.extend((grandchild, inner, inlined) for grandchild in grandchildren)
    return tuple(found)


def _build_function(unit: _Unit, source: _LocalsSource, die: DIE) -> Function | None:
    chain = _origin_chain(die)
    origin_die = chain[-1]

    name = _attr_str(origin_die, "DW_AT_name")
    if name is None:
        return None

    params: list[Parameter] = []
    variadic = False
    for child in _safe_children(origin_die):
        if child.tag == "DW_TAG_formal_parameter":
            type_id = _type_ref(child)
            if type_id is not None:
                params.append(Parameter(name=_attr_str(child, "DW_AT_name"), type=type_id))
        elif child.tag == "DW_TAG_unspecified_parameters":
            variadic = True

    address = _attr_int(die, "DW_AT_low_pc")
    if address is None:
        address = address_from_ranges(unit.cu, die)
    has_code = address is not None

    return Function(
        name=name,
        address=address,
        returns=_type_ref(origin_die),
        params=tuple(params),
        variadic=variadic,
        decl=_chain_decl(chain),
        frame_base=_frame_base(unit, die) if has_code else None,
        _locals=_LazyLocals(source, die.offset) if has_code else None,
    )


type _FileIdentity = tuple[int, int, int, int]


def _file_identity(descriptor: int) -> _FileIdentity:
    stat = os.fstat(descriptor)
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)


class _LocalsSource:
    """The ELF one load read, reopened on the first access to a function's locals. Its DWARF
    sections then stay in memory, each unit's lists are parsed once and each function's locals
    are kept. A lock serialises the reads, pyelftools streams are not thread-safe."""

    __slots__ = ("_collected", "_dwarf", "_identity", "_lock", "_path", "_units")

    def __init__(self, path: Path, identity: _FileIdentity) -> None:
        self._path = path
        self._identity = identity
        self._lock = threading.Lock()
        self._dwarf: DWARFInfo | None = None
        self._units: dict[int, _Unit] = {}
        self._collected: dict[int, tuple[Local, ...]] = {}

    def __reduce__(self) -> tuple[type[_LocalsSource], tuple[Path, _FileIdentity]]:
        return (_LocalsSource, (self._path, self._identity))

    def locals_at(self, offset: int) -> tuple[Local, ...]:
        with self._lock:
            found = self._collected.get(offset)
            if found is not None:
                return found
            try:
                found = self._collect(offset)
            except DebugInfoError:
                raise
            except OSError as exc:
                raise DebugInfoError(f"{self._path}: {exc}") from exc
            except _DECODE_ERRORS as exc:
                raise DebugInfoError(f"{self._path}: malformed DWARF: {exc!r}") from exc
            self._collected[offset] = found
            return found

    def _collect(self, offset: int) -> tuple[Local, ...]:
        if self._dwarf is None:
            with self._path.open("rb") as stream:
                if _file_identity(stream.fileno()) != self._identity:
                    raise DebugInfoError(f"{self._path}: changed since its debug info was read")
                self._dwarf = ELFFile(stream).get_dwarf_info()
        die = self._dwarf.get_DIE_from_refaddr(offset)
        cu = die.cu
        if not isinstance(cu, CompileUnit):
            return ()
        unit = self._units.get(cu.cu_offset)
        if unit is None:
            unit = _Unit(cu, LocationParser(cu.dwarfinfo.location_lists()))
            self._units[cu.cu_offset] = unit
        return _collect_locals(unit, die)


class _LazyLocals:
    """The locals of the function DIE at `offset`, read from `source` on first access."""

    __slots__ = ("_offset", "_source")

    def __init__(self, source: _LocalsSource, offset: int) -> None:
        self._source = source
        self._offset = offset

    def __reduce__(self) -> tuple[type[_LazyLocals], tuple[_LocalsSource, int]]:
        return (_LazyLocals, (self._source, self._offset))

    def get(self) -> tuple[Local, ...]:
        return self._source.locals_at(self._offset)


def _walk_cu(
    cu: CompileUnit,
    by_id: dict[TypeId, DebugType],
    types: dict[str, TypeId],
    type_is_decl: dict[str, bool],
    variable_candidates: dict[str, list[tuple[Variable, bool]]],
    functions: dict[str, Function],
    function_is_decl: dict[str, bool],
    source: _LocalsSource,
) -> None:
    top = cu.get_top_DIE()
    comp_dir = _attr_str(top, "DW_AT_comp_dir") or ""
    unit = _Unit(cu, LocationParser(cu.dwarfinfo.location_lists()))

    stack: list[tuple[DIE, bool]] = [(child, True) for child in _safe_children(top)]
    seen: set[int] = set()
    while stack:
        die, module_scope = stack.pop()
        if die.offset in seen:
            continue
        seen.add(die.offset)

        if die.tag in _TYPE_TAGS:
            _record_type(cu, comp_dir, die, by_id, types, type_is_decl)
        elif module_scope and die.tag == "DW_TAG_variable":
            built = _build_variable(die)
            if built is not None:
                name, variable, external = built
                variable_candidates.setdefault(name, []).append((variable, external))
        elif module_scope and die.tag == "DW_TAG_subprogram":
            function = _build_function(unit, source, die)
            if function is not None:
                is_decl = die.attributes.get("DW_AT_declaration") is not None
                _merge_function(functions, function_is_decl, function, is_decl)

        for child in _safe_children(die):
            stack.append((child, False))


def _fill_addresses_from_symtab(elf: ELFFile, functions: dict[str, Function]) -> None:
    """Ignores symbol binding; an ambiguous name match leaves the function's address None."""
    missing = {name for name, fn in functions.items() if fn.address is None}
    if not missing:
        return
    symtab = elf.get_section_by_name(".symtab")
    if not isinstance(symtab, SymbolTableSection):
        return

    candidates: dict[str, set[int]] = {}
    for sym in symtab.iter_symbols():
        if sym.name not in missing:
            continue
        if sym["st_info"]["type"] != "STT_FUNC" or sym["st_shndx"] == "SHN_UNDEF":
            continue
        candidates.setdefault(sym.name, set()).add(sym["st_value"])

    for name, values in candidates.items():
        if len(values) != 1:
            continue
        functions[name] = replace(functions[name], address=next(iter(values)) & ~1)


_DECODE_ERRORS: tuple[type[Exception], ...] = (
    ELFError,
    DWARFError,
    ConstructError,
    KeyError,
    IndexError,
    TypeError,
    AttributeError,
    AssertionError,
    struct.error,
    NotImplementedError,
    ValueError,
)


def _load(path: Path) -> DebugInfo:
    try:
        with path.open("rb") as stream:
            source = _LocalsSource(path, _file_identity(stream.fileno()))
            elf = ELFFile(stream)
            dwarf_info = elf.get_dwarf_info()

            by_id: dict[TypeId, DebugType] = {}
            types: dict[str, TypeId] = {}
            type_is_decl: dict[str, bool] = {}
            variable_candidates: dict[str, list[tuple[Variable, bool]]] = {}
            functions: dict[str, Function] = {}
            function_is_decl: dict[str, bool] = {}

            cus = list(dwarf_info.iter_CUs())
            if not cus:
                raise NoDebugInfo(f"{path}: no .debug_info compile units")
            units: list[DwarfUnit] = []
            for cu in cus:
                _walk_cu(
                    cu,
                    by_id,
                    types,
                    type_is_decl,
                    variable_candidates,
                    functions,
                    function_is_decl,
                    source,
                )
                name = _attr_str(cu.get_top_DIE(), "DW_AT_name") or ""
                units.append(DwarfUnit(name, cu.cu_offset, cu.cu_offset + cu.size))
            _fill_addresses_from_symtab(elf, functions)

            variables = _resolve_variables(variable_candidates)
            return DebugInfo(
                by_id=by_id,
                types=types,
                variables=variables,
                functions=functions,
                units=tuple(sorted(units, key=lambda unit: unit.start)),
            )
    except DebugInfoError:
        raise
    except OSError as exc:
        raise DebugInfoError(f"{path}: {exc}") from exc
    except _DECODE_ERRORS as exc:
        raise DebugInfoError(f"{path}: malformed DWARF: {exc!r}") from exc


@functools.lru_cache(maxsize=4)
def _load_cached(resolved: str, mtime_ns: int, size: int) -> DebugInfo:
    return _load(Path(resolved))


def load_debug_info(elf: Path) -> DebugInfo:
    resolved = Path(elf).resolve()
    try:
        stat = resolved.stat()
    except OSError as exc:
        raise DebugInfoError(f"{elf}: {exc}") from exc
    return _load_cached(str(resolved), stat.st_mtime_ns, stat.st_size)
