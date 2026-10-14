"""Guest function calls: argument kinds, signatures from DWARF, results."""

from __future__ import annotations

import enum
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

from popcorn import _core
from popcorn._enums import ValueKind
from popcorn._stop import Stop

if TYPE_CHECKING:
    from popcorn.image.dwarf import DebugInfo, Function, TypeId


class Stack(enum.Enum):
    """The stack pointer a call runs on."""

    CURRENT = "current"
    MAIN = "main"
    PROCESS = "process"


@dataclass(frozen=True, slots=True)
class I64:
    """A 64-bit integer argument, for a function without DWARF: `popcorn.i64(v)`."""

    value: int


@dataclass(frozen=True, slots=True)
class F32:
    """A single-precision float argument, for a function without DWARF: `popcorn.f32(v)`."""

    value: float


def i64(value: int) -> I64:
    """Pass `value` as a 64-bit integer to a function without DWARF."""
    return I64(value)


def f32(value: float) -> F32:
    """Pass `value` as a single-precision float to a function without DWARF."""
    return F32(value)


@dataclass(frozen=True, slots=True)
class CallResult:
    """What a call returned.

    Attributes:
        value: The return value, an int for integer and pointer returns, a float for float and
            double returns, 0 when the call did not return.
        value64: The raw bits of the return value.
        stop: How the call ended, RETURNED when it returned.
        ipsr: The exception number active when the call began, 0 in thread mode. For a call
            that never began, the one at its stop.
        began: Whether the call began. False when a `from_thread=True` wait stopped first.
    """

    value: int | float
    value64: int
    stop: Stop
    ipsr: int = 0
    began: bool = True


class HandlerCallWarning(UserWarning):
    """A call began with an exception handler active, so the handler resumes after it returns,
    which a thread-mode caller on hardware never sees. `call(from_thread=True)` waits for thread
    mode, `from_thread=False` calls at once without this warning."""


@dataclass(frozen=True, slots=True)
class Slot:
    """How one argument or the return value crosses: its kind, signedness and C byte size."""

    kind: _core.ValueKind
    signed: bool
    size: int = 4


I32_SLOT = Slot(_core.ValueKind.i32, True)


def slot_for(kind: ValueKind) -> Slot:
    """The slot of a result kind given without DWARF."""
    native = _core.ValueKind(kind.value)
    return Slot(native, True, 8 if kind in (ValueKind.I64, ValueKind.F64) else 4)


def slot_of(info: DebugInfo, type_id: TypeId | None, where: str) -> Slot:
    """The slot of a DWARF type, `where` naming it in errors."""
    from popcorn.image.dwarf import ArrayType, BaseType, Encoding, EnumType, PointerType

    if type_id is None:
        return I32_SLOT
    resolved = info.strip(type_id)
    match resolved:
        case BaseType(encoding=Encoding.FLOAT, size=4):
            return Slot(_core.ValueKind.f32, True, 4)
        case BaseType(encoding=Encoding.FLOAT, size=8):
            return Slot(_core.ValueKind.f64, True, 8)
        case BaseType(encoding=encoding, size=size):
            kind = _core.ValueKind.i64 if size == 8 else _core.ValueKind.i32
            return Slot(kind, encoding is Encoding.SIGNED, size)
        case EnumType(size=size, values=values):
            kind = _core.ValueKind.i64 if size == 8 else _core.ValueKind.i32
            return Slot(kind, any(v < 0 for v in values.values()), size)
        case PointerType() | ArrayType():
            return Slot(_core.ValueKind.i32, False, 4)
        case _:
            raise NotImplementedError(
                f"{where} is passed by value as {type(resolved).__name__}, pass structs by pointer"
            )


@dataclass(frozen=True, slots=True)
class Signature:
    """Parameter and return slots of a function."""

    name: str
    params: tuple[Slot, ...] | None
    returns: Slot
    void: bool = False
    unsupported: str | None = None
    returns_unsupported: str | None = None


def signature_of(info: DebugInfo, function: Function) -> Signature:
    """The signature DWARF gives a function. A parameter popcorn cannot pass leaves the
    parameters unknown, the reason kept in `unsupported`."""
    try:
        returns = slot_of(info, function.returns, function.name)
    except NotImplementedError as error:
        return Signature(function.name, None, I32_SLOT, False, str(error), str(error))
    try:
        params = tuple(
            slot_of(info, p.type, f"parameter {p.name or index} of {function.name}")
            for index, p in enumerate(function.params)
        )
    except NotImplementedError as error:
        return Signature(function.name, None, returns, function.returns is None, str(error))
    return Signature(function.name, params, returns, function.returns is None)


def argument(value: object, slot: Slot | None, where: str) -> _core.CallArg:
    """The native argument for `value`, of `slot` when the signature is known."""
    kind: _core.ValueKind
    number: int | float
    match value:
        case I64(value=v):
            kind, number = _core.ValueKind.i64, v
        case F32(value=v):
            kind, number = _core.ValueKind.f32, v
        case bool() | int():
            kind, number = _core.ValueKind.i32, int(value)
        case float():
            kind, number = _core.ValueKind.f64, value
        case _:
            address = getattr(value, "address", None)
            if not isinstance(address, int):
                raise TypeError(f"{where} cannot pass {value!r}: an int, a float or an address")
            kind, number = _core.ValueKind.i32, address
    if slot is not None:
        if isinstance(value, I64 | F32) and value_kind_name(slot.kind) != value_kind_name(kind):
            raise TypeError(f"{where} is {value_kind_name(slot.kind)}, not {value_kind_name(kind)}")
        kind = slot.kind
        if kind in (_core.ValueKind.i32, _core.ValueKind.i64):
            number = narrowed(slot, number, where)
    arg = _core.CallArg()
    arg.kind = kind
    arg.bits = encode(kind, number, where)
    return arg


def narrowed(slot: Slot, value: float, where: str) -> int:
    """`value` range-checked against the parameter's C type, sign- or zero-extended to its
    register width."""
    if not isinstance(value, int):
        raise TypeError(f"{where} is an integer, not {value!r}")
    bits = slot.size * 8
    low, high = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if slot.signed else (0, (1 << bits) - 1)
    if not low <= value <= high:
        kind = f"{'' if slot.signed else 'u'}int{bits}_t"
        raise ValueError(f"{where} is a {kind}, {value} is outside {low} to {high}")
    return value


def value_kind_name(kind: _core.ValueKind) -> str:
    """The C name of a kind."""
    return {"i32": "int32", "i64": "int64", "f32": "float", "f64": "double"}[kind.name]


def encode(kind: _core.ValueKind, value: float, where: str) -> int:
    """The bits of `value` as `kind`."""
    if kind in (_core.ValueKind.f32, _core.ValueKind.f64):
        fmt = "<f" if kind is _core.ValueKind.f32 else "<d"
        return int.from_bytes(struct.pack(fmt, float(value)), "little")
    if not isinstance(value, int):
        raise TypeError(f"{where} is an integer, not {value!r}")
    bits = 64 if kind is _core.ValueKind.i64 else 32
    if not -(1 << (bits - 1)) <= value < 1 << bits:
        raise ValueError(f"{where}: {value} does not fit {bits} bits")
    return value & ((1 << bits) - 1)


def decode(slot: Slot, bits: int) -> int | float:
    """The Python value of return bits."""
    match slot.kind.name:
        case "f32":
            value: float = struct.unpack("<f", (bits & 0xFFFF_FFFF).to_bytes(4, "little"))[0]
            return value
        case "f64":
            wide: float = struct.unpack("<d", bits.to_bytes(8, "little"))[0]
            return wide
        case _:
            width = slot.size * 8
            value_bits = bits & ((1 << width) - 1)
            if slot.signed and value_bits >> (width - 1):
                return value_bits - (1 << width)
            return value_bits
