"""A mirror of the MDF and ADF model's saved state, `mdf_state` of `mdf.cpp`, for tests.

`decode` reads a model's snapshot bytes into the mirror and refuses any key, element type or
order it does not expect. `encode` writes the mirror back, so a test that requires
`encode(decode(saved)) == saved` sees a field the model added, removed, retyped or moved.
"""

from __future__ import annotations

import dataclasses
import struct
import types
from dataclasses import dataclass
from typing import Annotated, Any, Union, cast, get_args, get_origin, get_type_hints


@dataclass(frozen=True)
class Kind:
    """BSON element type of an integer field: 0x10 int32 or 0x12 int64."""

    code: int


I32 = Annotated[int, Kind(0x10)]
I64 = Annotated[int, Kind(0x12)]
DOCUMENT = 0x03
ARRAY = 0x04
BOOL = 0x08
NULL = 0x0A


@dataclass
class Cck:
    running: bool
    anchor_ps: I64
    base: I64
    kernel_hz: I64
    divider: I64


@dataclass
class Modulator:
    i1: I64
    i2: I64


@dataclass
class Sitf:
    clocking: bool
    next: I64
    silence: list[Modulator]


@dataclass
class Config:
    order: I32
    decimation: I32
    offset: I32
    scale: I32
    rsflt: bool
    rsflt_decimate: bool
    hpf: I32 | None
    int_value: I32 | None
    int_div: I32


@dataclass
class Cic:
    integ: list[I64]
    comb: list[I64]
    fast: list[I64]
    count: I32


@dataclass
class Biquad:
    x: list[I64]
    y: list[I64]


@dataclass
class Rsflt:
    stage: list[Biquad]
    count: I32


@dataclass
class Hpf:
    x1: I64
    y1: I64


@dataclass
class Integrator:
    sum: I64
    count: I32
    saturated: bool


@dataclass
class Chain:
    cic: Cic
    rsflt: Rsflt
    hpf: Hpf
    integ: Integrator
    saturated: bool


@dataclass
class Filter:
    ckab_generation: I64
    running: bool
    single: bool
    dma: bool
    rsflt_fed: bool
    discard: I32
    carried: bool
    skip: I32
    config: Config
    chain: Chain
    fifo: list[I32]


@dataclass
class SadSetup:
    mode: I32
    frame: I32
    learn: I32
    hangover: I32
    slope: I32
    threshold: I32
    hysteresis: bool
    floor: I32


@dataclass
class Sad:
    phase: I32
    sum: I64
    count: I32
    learned: I32
    learn_sum: I64
    noise: I64
    hang: I32


@dataclass
class MdfState:
    filter: list[Filter]
    sitf: list[Sitf]
    cck: list[Cck]
    sample_generation: I64
    scheduled_ps: I64 | None
    round: I32
    sad_setup: SadSetup
    sad: Sad


type Element = tuple[int, object]


def elements(data: bytes) -> list[tuple[str, Element]]:
    """The elements of the BSON document @p data, nested documents and arrays as lists."""

    def document(at: int) -> list[tuple[str, Element]]:
        (size,) = struct.unpack_from("<i", data, at)
        end = at + size - 1
        at += 4
        out: list[tuple[str, Element]] = []
        while at < end:
            code = data[at]
            name_end = data.index(0, at + 1)
            name = data[at + 1 : name_end].decode()
            at = name_end + 1
            value: object
            if code in (DOCUMENT, ARRAY):
                value = document(at)
                at += struct.unpack_from("<i", data, at)[0]
            elif code == 0x12:
                (value,) = struct.unpack_from("<q", data, at)
                at += 8
            elif code == 0x10:
                (value,) = struct.unpack_from("<i", data, at)
                at += 4
            elif code == BOOL:
                value = data[at] != 0
                at += 1
            elif code == NULL:
                value = None
            else:
                raise ValueError(f"BSON element type {code:#x} at {name}")
            out.append((name, (code, value)))
        if data[end] != 0 or at != end:
            raise ValueError("BSON document not terminated where its size says")
        return out

    return document(0)


def _decode(hint: object, element: Element, path: str) -> object:
    code, value = element
    origin = get_origin(hint)
    if origin in (Union, types.UnionType):
        if code == NULL:
            return None
        (inner,) = [arg for arg in get_args(hint) if arg is not type(None)]
        return _decode(inner, element, path)
    if origin is Annotated:
        expected = cast(Kind, get_args(hint)[1]).code
        if code != expected:
            raise ValueError(f"{path}: element type {code:#x}, mirror says {expected:#x}")
        return value
    if hint is bool:
        if code != BOOL:
            raise ValueError(f"{path}: element type {code:#x}, mirror says bool")
        return value
    if origin is list:
        if code != ARRAY:
            raise ValueError(f"{path}: element type {code:#x}, mirror says an array")
        (inner,) = get_args(hint)
        items = cast(list[tuple[str, Element]], value)
        if [name for name, _ in items] != [str(k) for k in range(len(items))]:
            raise ValueError(f"{path}: array keys out of order")
        return [_decode(inner, item, f"{path}[{name}]") for name, item in items]
    if code != DOCUMENT:
        raise ValueError(f"{path}: element type {code:#x}, mirror says a document")
    return _decode_document(cast(type, hint), cast(list[tuple[str, Element]], value), path)


def _decode_document(kind: type, items: list[tuple[str, Element]], path: str) -> object:
    hints = get_type_hints(kind, include_extras=True)
    names = [field.name for field in dataclasses.fields(kind)]
    if [name for name, _ in items] != names:
        raise ValueError(f"{path or 'state'}: keys {[name for name, _ in items]}, mirror {names}")
    values = {name: _decode(hints[name], item, f"{path}.{name}") for name, item in items}
    return kind(**values)


def decode(saved: bytes) -> MdfState:
    """The mirror of the model state bytes @p saved."""
    return cast(MdfState, _decode_document(MdfState, elements(saved), ""))


def _cstring(name: str) -> bytes:
    return name.encode() + b"\0"


def _encode(hint: object, value: object, name: str) -> bytes:
    origin = get_origin(hint)
    if origin in (Union, types.UnionType):
        if value is None:
            return bytes([NULL]) + _cstring(name)
        (inner,) = [arg for arg in get_args(hint) if arg is not type(None)]
        return _encode(inner, value, name)
    if origin is Annotated:
        code = cast(Kind, get_args(hint)[1]).code
        packed = struct.pack("<i" if code == 0x10 else "<q", cast(int, value))
        return bytes([code]) + _cstring(name) + packed
    if hint is bool:
        return bytes([BOOL]) + _cstring(name) + bytes([1 if value else 0])
    if origin is list:
        (inner,) = get_args(hint)
        items = cast(list[object], value)
        body = b"".join(_encode(inner, item, str(k)) for k, item in enumerate(items))
        return bytes([ARRAY]) + _cstring(name) + _frame(body)
    return bytes([DOCUMENT]) + _cstring(name) + _encode_document(value)


def _frame(body: bytes) -> bytes:
    return struct.pack("<i", len(body) + 5) + body + b"\0"


def _encode_document(value: object) -> bytes:
    hints = get_type_hints(type(value), include_extras=True)
    fields: dict[str, Any] = {
        field.name: getattr(value, field.name) for field in dataclasses.fields(cast(Any, value))
    }
    return _frame(b"".join(_encode(hints[name], item, name) for name, item in fields.items()))


def encode(state: MdfState) -> bytes:
    """The BSON bytes of @p state, as the model saves them."""
    return _encode_document(state)
