"""State encoding shared by Python models and bus targets: the declared state fields, in
declaration order, as bytes a snapshot keeps."""

from __future__ import annotations

import struct
import typing
from typing import ClassVar

from popcorn._errors import ModelError

STATE_TYPES: tuple[type, ...] = (bool, int, float, bytes, bytearray, str)
"""Types of the annotated fields that default snapshots save."""


def declared_fields(
    cls: type, base: type, *, custom: bool, other: tuple[object, ...] = ()
) -> tuple[tuple[tuple[str, type], ...], tuple[str, ...]]:
    """The annotated fields `cls` adds to `base`, ClassVars aside: the state fields of
    STATE_TYPES in declaration order, and the names of the fields annotated with a type of
    `other`.

    Raises:
        TypeError: An annotation does not resolve, or a field of another type is declared while
            `custom` is False: the class does not override snapshot() and restore().
    """
    try:
        hints = typing.get_type_hints(cls)
    except NameError as error:
        raise TypeError(f"{cls.__qualname__}: annotations must resolve: {error}") from error
    inherited = typing.get_type_hints(base)
    state: list[tuple[str, type]] = []
    others: list[str] = []
    for name, hint in hints.items():
        if name in inherited or typing.get_origin(hint) is ClassVar:
            continue
        if any(hint is kind for kind in other):
            others.append(name)
        elif hint in STATE_TYPES:
            state.append((name, typing.cast(type, hint)))
        elif not custom:
            raise TypeError(
                f"{cls.__qualname__}.{name}: state of type {hint} is not snapshotted by "
                "default, override snapshot() and restore()"
            )
    return tuple(state), tuple(others)


def encode(out: bytearray, value: object) -> None:
    """Append `value`, one of STATE_TYPES, to `out`."""
    if isinstance(value, bool):
        out += struct.pack("<?", value)
    elif isinstance(value, int):
        raw = value.to_bytes((value.bit_length() + 8) // 8, "little", signed=True)
        out += struct.pack("<H", len(raw)) + raw
    elif isinstance(value, float):
        out += struct.pack("<d", value)
    else:
        raw = value.encode() if isinstance(value, str) else bytes(typing.cast(bytes, value))
        out += struct.pack("<I", len(raw)) + raw


class StateReader:
    """Reads back the values `encode` wrote, in order."""

    __slots__ = ("_at", "_data")

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._at = 0

    def done(self) -> bool:
        return self._at == len(self._data)

    def read(self, kind: type) -> object:
        if kind is bool:
            return self._unpack("<?")
        if kind is float:
            return self._unpack("<d")
        if kind is int:
            return int.from_bytes(self._take(self._length("<H")), "little", signed=True)
        raw = self._take(self._length("<I"))
        if kind is str:
            return raw.decode()
        return bytearray(raw) if kind is bytearray else raw

    def _unpack(self, layout: str) -> object:
        value: object = struct.unpack(layout, self._take(struct.calcsize(layout)))[0]
        return value

    def _length(self, layout: str) -> int:
        return typing.cast(int, self._unpack(layout))

    def _take(self, count: int) -> bytes:
        if self._at + count > len(self._data):
            raise ModelError(f"model state ends after {len(self._data)} bytes")
        chunk = self._data[self._at : self._at + count]
        self._at += count
        return chunk
