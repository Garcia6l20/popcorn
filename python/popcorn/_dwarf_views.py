"""DWARF-typed views of guest memory: `m.var("table").entries[3].target.value`."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from popcorn._views import Memory

if TYPE_CHECKING:
    from popcorn.image.dwarf import DebugInfo, DebugType, Member, TypeId


def size_of(info: DebugInfo, type_id: TypeId) -> int:
    """Size in bytes of a type."""
    from popcorn.image.dwarf import ArrayType, FunctionType, Qualified, Typedef

    resolved = info.strip(type_id)
    match resolved:
        case ArrayType(element=element, counts=counts):
            size = size_of(info, element)
            for count in counts:
                size *= count or 0
            return size
        case FunctionType():
            raise TypeError("a function type has no size")
        case Typedef() | Qualified():
            raise TypeError("an unresolved type has no size")
        case _:
            return resolved.size


@dataclass(frozen=True, slots=True)
class _Bits:
    offset: int
    size: int


class Var:
    """A typed location in guest memory.

    A struct or union member is an attribute, an array element an index, a pointer is followed
    with `deref()`. `value` reads and writes a scalar: an integer, a float, a bool, an enum as
    its integer, a pointer as its address.
    """

    __slots__ = ("_bits", "_counts", "_info", "_memory", "_name", "_type_id", "address")

    def __init__(
        self,
        memory: Memory,
        info: DebugInfo,
        type_id: TypeId,
        address: int,
        name: str,
        *,
        counts: tuple[int | None, ...] | None = None,
        bits: _Bits | None = None,
    ) -> None:
        self._memory = memory
        self._info = info
        self._type_id = type_id
        self.address = address
        self._name = name
        self._counts = counts
        self._bits = bits

    @property
    def name(self) -> str:
        """Path of the location from its variable, such as `table.entries[3]`."""
        return self._name

    @property
    def info(self) -> DebugInfo:
        """The DWARF the type comes from."""
        return self._info

    @property
    def type_id(self) -> TypeId:
        """The type, as an id into `info`."""
        return self._type_id

    @property
    def type(self) -> DebugType:
        """The type, typedefs and qualifiers stripped."""
        return self._info.strip(self._type_id)

    @property
    def size(self) -> int:
        """Size in bytes."""
        from popcorn.image.dwarf import ArrayType

        if self._counts is not None:
            array = self.type
            assert isinstance(array, ArrayType)
            size = size_of(self._info, array.element)
            for count in self._counts:
                size *= count or 0
            return size
        return size_of(self._info, self._type_id)

    @property
    def fields(self) -> tuple[str, ...]:
        """Member names of a struct or union, empty otherwise."""
        from popcorn.image.dwarf import StructType

        resolved = self.type
        return tuple(m.name for m in resolved.members) if isinstance(resolved, StructType) else ()

    def __len__(self) -> int:
        counts = self._dims()
        if counts[0] is None:
            raise TypeError(f"{self._name} has no length")
        return counts[0]

    def _dims(self) -> tuple[int | None, ...]:
        from popcorn.image.dwarf import ArrayType

        resolved = self.type
        if not isinstance(resolved, ArrayType):
            raise TypeError(f"{self._name} is not an array")
        return self._counts if self._counts is not None else resolved.counts

    def _member(self, name: str) -> Member:
        from popcorn.image.dwarf import StructType

        resolved = self.type
        if not isinstance(resolved, StructType):
            raise AttributeError(f"{self._name} is not a struct or union, it has no {name}")
        for member in resolved.members:
            if member.name == name:
                return member
        raise AttributeError(
            f"{self._name} has no member {name}, members: {', '.join(self.fields)}"
        )

    def __getattr__(self, name: str) -> Var:
        member = self._member(name)
        bits = None
        if member.bit_size is not None:
            bits = _Bits(member.bit_offset or 0, member.bit_size)
        return Var(
            self._memory,
            self._info,
            member.type,
            self.address + member.offset,
            f"{self._name}.{name}",
            bits=bits,
        )

    def __getitem__(self, index: int) -> Var:
        from popcorn.image.dwarf import ArrayType

        counts = self._dims()
        array = self.type
        assert isinstance(array, ArrayType)
        if counts[0] is not None and not 0 <= index < counts[0]:
            raise IndexError(f"{self._name}[{index}] is outside {counts[0]} elements")
        rest = counts[1:]
        stride = size_of(self._info, array.element)
        for count in rest:
            stride *= count or 0
        name = f"{self._name}[{index}]"
        address = self.address + index * stride
        if rest:
            return Var(self._memory, self._info, self._type_id, address, name, counts=rest)
        return Var(self._memory, self._info, array.element, address, name)

    def deref(self) -> Var:
        """The location a pointer points to."""
        from popcorn.image.dwarf import PointerType

        resolved = self.type
        if not isinstance(resolved, PointerType):
            raise TypeError(f"{self._name} is not a pointer")
        if resolved.target is None:
            raise TypeError(f"{self._name} is a void pointer")
        target = self.value
        assert isinstance(target, int)
        return Var(self._memory, self._info, resolved.target, target, f"*{self._name}")

    @property
    def enum_name(self) -> str | None:
        """Name of the enumerator an enum holds, None when no enumerator has its value."""
        from popcorn.image.dwarf import EnumType

        resolved = self.type
        if not isinstance(resolved, EnumType):
            raise TypeError(f"{self._name} is not an enum")
        value = self.value
        return next((k for k, v in resolved.values.items() if v == value), None)

    def _scalar(self) -> tuple[str, int]:
        from popcorn.image.dwarf import BaseType, Encoding, EnumType, PointerType

        resolved = self.type
        match resolved:
            case BaseType(encoding=Encoding.FLOAT, size=4):
                return "<f", 4
            case BaseType(encoding=Encoding.FLOAT, size=8):
                return "<d", 8
            case BaseType(size=size) | EnumType(size=size) if self._signed():
                return "<" + {1: "b", 2: "h", 4: "i", 8: "q"}[size], size
            case BaseType(size=size) | EnumType(size=size) | PointerType(size=size):
                return "<" + {1: "B", 2: "H", 4: "I", 8: "Q"}[size], size
            case _:
                raise TypeError(f"{self._name} is not a scalar, read its members or elements")

    def _signed(self) -> bool:
        from popcorn.image.dwarf import BaseType, Encoding, EnumType

        resolved = self.type
        if isinstance(resolved, EnumType):
            return any(value < 0 for value in resolved.values.values())
        return isinstance(resolved, BaseType) and resolved.encoding is Encoding.SIGNED

    def span(self) -> int:
        """Bytes the scalar occupies from `address`, its bit field included.

        Raises:
            TypeError: The location is not a scalar.
        """
        _, size = self._scalar()
        if self._bits is not None:
            return (self._bits.offset + self._bits.size + 7) // 8
        return size

    def decode(self, data: bytes) -> int | float | bool:
        """The scalar value of the `span()` bytes read at `address`."""
        from popcorn.image.dwarf import BaseType, Encoding

        fmt, _ = self._scalar()
        if self._bits is not None:
            word = int.from_bytes(data, "little")
            raw = (word >> self._bits.offset) & ((1 << self._bits.size) - 1)
            if self._signed() and raw >> (self._bits.size - 1):
                raw -= 1 << self._bits.size
            return raw
        value: int | float = struct.unpack(fmt, data)[0]
        resolved = self.type
        if isinstance(resolved, BaseType) and resolved.encoding is Encoding.BOOLEAN:
            return bool(value)
        return value

    def encode(self, value: int | float | bool) -> tuple[bytes, bytes | None]:
        """The bytes written at `address` for `value`, and for a bit field the mask of the bits
        they set, None for a whole scalar.

        Raises:
            TypeError: A bit field given a non-int.
            ValueError: `value` does not fit.
        """
        fmt, size = self._scalar()
        if self._bits is not None:
            if not isinstance(value, int):
                raise TypeError(f"{self._name} is a bitfield, it takes an int")
            low, high = (
                (-(1 << (self._bits.size - 1)), (1 << (self._bits.size - 1)) - 1)
                if self._signed()
                else (0, (1 << self._bits.size) - 1)
            )
            if not low <= value <= high:
                raise ValueError(
                    f"{value} does not fit the {self._bits.size}-bit field {self._name},"
                    f" {low} to {high}"
                )
            span = self.span()
            mask = ((1 << self._bits.size) - 1) << self._bits.offset
            word = (value << self._bits.offset) & mask
            return word.to_bytes(span, "little"), mask.to_bytes(span, "little")
        try:
            data = struct.pack(fmt, value)
        except (struct.error, OverflowError) as error:
            raise ValueError(f"{value!r} does not fit {self._name}: {error}") from error
        assert len(data) == size
        return data, None

    @property
    def value(self) -> int | float | bool:
        """The scalar value."""
        return self.decode(self._memory.read(self.address, self.span()))

    @value.setter
    def value(self, value: int | float | bool) -> None:
        data, mask = self.encode(value)
        if mask is None:
            self._memory.write(self.address, data)
        else:
            self._memory.write_masked(self.address, mask, data)

    def __repr__(self) -> str:
        return f"<Var {self._name} @ {self.address:#010x}>"


@dataclass(frozen=True, slots=True)
class ScalarLayout:
    """Where a scalar lies in memory: `size` bytes read little-endian at `address`, shifted right
    by `shift`, kept to `bits` bits."""

    address: int
    size: int
    shift: int
    bits: int
    kind: Literal["unsigned", "signed", "float", "bool"]


def scalar_layout(var: Var) -> ScalarLayout:
    """The layout of the scalar `var`, a bit field included.

    Raises:
        TypeError: `var` is not a scalar.
    """
    from popcorn.image.dwarf import BaseType, Encoding

    fmt, size = var._scalar()  # pyright: ignore[reportPrivateUsage]
    bits = var._bits  # pyright: ignore[reportPrivateUsage]
    signed = var._signed()  # pyright: ignore[reportPrivateUsage]
    if bits is not None:
        return ScalarLayout(
            var.address,
            (bits.offset + bits.size + 7) // 8,
            bits.offset,
            bits.size,
            "signed" if signed else "unsigned",
        )
    resolved = var.type
    kind: Literal["unsigned", "signed", "float", "bool"]
    if fmt in ("<f", "<d"):
        kind = "float"
    elif isinstance(resolved, BaseType) and resolved.encoding is Encoding.BOOLEAN:
        kind = "bool"
    else:
        kind = "signed" if signed else "unsigned"
    return ScalarLayout(var.address, size, 0, 8 * size, kind)
