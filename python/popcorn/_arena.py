"""The call arena: scratch memory outside firmware RAM for call arguments."""

from __future__ import annotations

import ctypes
import struct
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING

from popcorn._errors import ConfigError
from popcorn._views import Memory

if TYPE_CHECKING:
    from popcorn._dwarf_views import Var
    from popcorn.image.dwarf import DebugType

_FORMATS: dict[type, str] = {
    ctypes.c_int8: "b",
    ctypes.c_uint8: "B",
    ctypes.c_int16: "h",
    ctypes.c_uint16: "H",
    ctypes.c_int32: "i",
    ctypes.c_uint32: "I",
    ctypes.c_int64: "q",
    ctypes.c_uint64: "Q",
    ctypes.c_float: "f",
    ctypes.c_double: "d",
}

type CScalar = (
    type[ctypes.c_int8]
    | type[ctypes.c_uint8]
    | type[ctypes.c_int16]
    | type[ctypes.c_uint16]
    | type[ctypes.c_int32]
    | type[ctypes.c_uint32]
    | type[ctypes.c_int64]
    | type[ctypes.c_uint64]
    | type[ctypes.c_float]
    | type[ctypes.c_double]
)
"""A ctypes scalar type an arena array can hold."""


class ArenaBlock:
    """A block of the arena. Passed to a call it is its address, `int(block)` too."""

    __slots__ = ("_format", "_memory", "_var", "address", "size")

    def __init__(
        self,
        memory: Memory,
        address: int,
        size: int,
        *,
        fmt: str | None = None,
        var: Var | None = None,
    ) -> None:
        self._memory = memory
        self.address = address
        self.size = size
        self._format = fmt
        self._var = var

    def __int__(self) -> int:
        return self.address

    def __index__(self) -> int:
        return self.address

    @property
    def var(self) -> Var:
        """The block as a DWARF-typed variable, for a block allocated from a DWARF type."""
        if self._var is None:
            raise TypeError("the block was not allocated from a DWARF type")
        return self._var

    def read(self) -> bytes:
        """The bytes of the block."""
        return self._memory.read(self.address, self.size)

    def write(self, data: bytes | Sequence[int] | Sequence[float]) -> None:
        """Write bytes, or the values of an array block, from the start of the block.

        Args:
            data: Bytes, or the values of an array block.
        """
        raw = data if isinstance(data, bytes) else self._pack(data)
        if len(raw) > self.size:
            raise ValueError(f"{len(raw)} bytes do not fit the {self.size}-byte block")
        self._memory.write(self.address, raw)

    def values(self) -> list[int] | list[float]:
        """The elements of an array block."""
        if self._format is None:
            raise TypeError("the block is not an array, read its bytes")
        count = self.size // struct.calcsize(self._format)
        return list(struct.unpack(f"<{count}{self._format}", self.read()))

    def _pack(self, values: Sequence[int] | Sequence[float]) -> bytes:
        if self._format is None:
            raise TypeError("the block is not an array, write bytes")
        return struct.pack(f"<{len(values)}{self._format}", *values)

    def __repr__(self) -> str:
        return f"<ArenaBlock {self.address:#010x} {self.size} bytes>"


class Arena:
    """A bump allocator over the call arena, a RAM region outside the firmware's memory map.

    Part of every snapshot: `restore` returns the allocation offset to the one the snapshot
    recorded. A snapshot loaded from a file starts the arena empty. After a restore, or a
    `call(reset=True)`, a block allocated after that snapshot aliases the next allocations:
    drop it. The last 16 bytes hold the call return sentinel and are never allocated.
    """

    __slots__ = ("_find_type", "_memory", "_offset", "base", "size")

    def __init__(
        self,
        memory: Memory,
        base: int,
        size: int,
        find_type: Callable[[DebugType | str, int], Var | None],
    ) -> None:
        self._memory = memory
        self.base = base
        self.size = size
        self._offset = 0
        self._find_type = find_type

    @property
    def used(self) -> int:
        """Bytes allocated so far."""
        return self._offset

    @property
    def sentinel(self) -> int:
        """Return address of every call, reserved at the end of the arena."""
        return self.base + self.size - 16

    def reset(self, offset: int = 0) -> None:
        """Free every block allocated after `offset`."""
        self._offset = offset

    def _take(self, size: int, align: int) -> int:
        if size <= 0 or align <= 0 or align & (align - 1):
            raise ValueError(f"a positive size and a power-of-two alignment, not {size}, {align}")
        start = (self.base + self._offset + align - 1) & ~(align - 1)
        end = start + size
        if end > self.sentinel:
            raise ConfigError(
                f"the arena has {self.sentinel - self.base - self._offset} bytes left, not {size}",
                file="arena",
                key=f"{self.base:#010x}",
                expected=f"at most {self.size - 16} bytes of allocations",
            )
        self._offset = end - self.base
        return start

    def alloc(self, what: int | str | DebugType | Var, align: int = 8) -> ArenaBlock:
        """A zero-filled block of `what` bytes, or sized for a DWARF type, given as a type, a
        type name such as `"struct point"`, or a variable whose type to take, with a `var`
        view of the block.

        Args:
            what: The size in bytes, a DWARF type, a type name or a variable.
            align: Alignment of the block in bytes.

        Raises:
            TypeError: The type belongs to no loaded image's DWARF.
            ConfigError: The arena is full.
        """
        from popcorn._dwarf_views import Var, size_of

        if isinstance(what, int):
            address = self._take(what, align)
            block = ArenaBlock(self._memory, address, what)
        else:
            if isinstance(what, Var):
                probe: Var | None = what
            else:
                probe = self._find_type(what, 0)
            if probe is None:
                raise TypeError(f"{what!r} is no type of the loaded images' DWARF")
            size = size_of(probe.info, probe.type_id)
            address = self._take(size, align)
            view = Var(self._memory, probe.info, probe.type_id, address, probe.name)
            block = ArenaBlock(self._memory, address, size, var=view)
        self._memory.write(block.address, bytes(block.size))
        return block

    def alloc_array(self, ctype: CScalar, count: int) -> ArenaBlock:
        """A zero-filled array of `count` ctypes scalars, written and read as values.

        Args:
            ctype: The scalar type of the elements.
            count: Number of elements.
        """
        fmt = _FORMATS.get(ctype)
        if fmt is None:
            raise TypeError(f"{ctype.__name__} is not a ctypes scalar type")
        size = ctypes.sizeof(ctype) * count
        address = self._take(size, ctypes.alignment(ctype))
        self._memory.write(address, bytes(size))
        return ArenaBlock(self._memory, address, size, fmt=fmt)

    def bytes(self, data: bytes, align: int = 8) -> ArenaBlock:
        """A block holding `data`.

        Args:
            data: The bytes the block holds.
            align: Alignment of the block in bytes.
        """
        block = self.alloc(len(data), align)
        block.write(data)
        return block

    def cstring(self, text: str) -> ArenaBlock:
        """A block holding `text` as NUL-terminated UTF-8."""
        return self.bytes(text.encode() + b"\0", align=1)
