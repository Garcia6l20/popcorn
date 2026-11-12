"""A decoder of the BSON documents snapshots hold, for tests that compare them by key."""

from __future__ import annotations

import struct


def bson(data: bytes, at: int = 0) -> dict[str, object]:
    """The BSON document at `at`, with the element types snapshots use."""
    (size,) = struct.unpack_from("<i", data, at)
    end = at + size - 1
    at += 4
    found: dict[str, object] = {}
    while at < end:
        kind = data[at]
        name_end = data.index(0, at + 1)
        name = data[at + 1 : name_end].decode()
        at = name_end + 1
        value: object
        if kind in (0x03, 0x04):
            value = bson(data, at)
            at += struct.unpack_from("<i", data, at)[0]
        elif kind == 0x05:
            (length,) = struct.unpack_from("<i", data, at)
            value = data[at + 5 : at + 5 + length]
            at += 5 + length
        elif kind == 0x02:
            (length,) = struct.unpack_from("<i", data, at)
            value = data[at + 4 : at + 3 + length].decode()
            at += 4 + length
        elif kind == 0x01:
            (value,) = struct.unpack_from("<d", data, at)
            at += 8
        elif kind in (0x09, 0x11, 0x12):
            (value,) = struct.unpack_from("<q", data, at)
            at += 8
        elif kind == 0x10:
            (value,) = struct.unpack_from("<i", data, at)
            at += 4
        elif kind == 0x08:
            value = data[at] != 0
            at += 1
        elif kind == 0x0A:
            value = None
        else:
            raise ValueError(f"BSON element type {kind:#x} at {name}")
        found[name] = value
    return found


def length_fields(data: bytes, at: int = 0, nested: bool = False) -> dict[str, list[int]]:
    """The offsets of the int32 length fields of the string, binary and sub-document elements
    of the BSON document at `at`, by kind, in document order."""
    found: dict[str, list[int]] = {"string": [], "binary": [], "document": []}
    (size,) = struct.unpack_from("<i", data, at)
    if nested:
        found["document"].append(at)
    end = at + size - 1
    at += 4
    while at < end:
        kind = data[at]
        at = data.index(0, at + 1) + 1
        if kind in (0x03, 0x04):
            for name, offsets in length_fields(data, at, True).items():
                found[name].extend(offsets)
            at += struct.unpack_from("<i", data, at)[0]
        elif kind in (0x02, 0x05):
            found["string" if kind == 0x02 else "binary"].append(at)
            at += (4 if kind == 0x02 else 5) + struct.unpack_from("<i", data, at)[0]
        elif kind in (0x01, 0x09, 0x11, 0x12):
            at += 8
        elif kind == 0x10:
            at += 4
        elif kind == 0x08:
            at += 1
        elif kind != 0x0A:
            raise ValueError(f"BSON element type {kind:#x}")
    return found
