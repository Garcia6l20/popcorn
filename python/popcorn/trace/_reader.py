"""Reading and compressing .pctr trace files."""

from __future__ import annotations

import os
import struct
from collections.abc import Iterator
from contextlib import ExitStack
from io import BufferedIOBase
from pathlib import Path
from typing import Final

from popcorn.trace._records import (
    PAYLOAD_WORDS,
    Block,
    Call,
    Irq,
    Mem,
    Mmio,
    Record,
    Stop,
    TraceKind,
)

type PathLike = str | os.PathLike[str]

_MAGIC: Final = b"PCTR"
_VERSION: Final = 1
_ZSTD_MAGIC: Final = b"\x28\xb5\x2f\xfd"
_CHUNK: Final = 1 << 20
_HEADER: Final = struct.Struct("<BBHI")
_OVERFLOW: Final = 0xFFFF
_PAYLOADS: Final = {kind: struct.Struct(f"<{words}I") for kind, words in PAYLOAD_WORDS.items()}


class TraceFileError(ValueError):
    """A .pctr file is malformed."""


def _zstd_file(file: BufferedIOBase | Path, mode: str, level: int | None = None) -> BufferedIOBase:
    try:
        from compression import zstd
    except ImportError as error:
        raise TraceFileError(
            "a zstd-compressed trace needs Python's compression.zstd module"
        ) from error
    if mode == "rb":
        return zstd.ZstdFile(file, "rb")
    return zstd.ZstdFile(file, "wb", level=level)


def _zstd_errors() -> tuple[type[Exception], ...]:
    from compression import zstd

    return (EOFError, zstd.ZstdError)


def _open(path: Path, stack: ExitStack) -> tuple[BufferedIOBase, tuple[type[Exception], ...]]:
    raw = stack.enter_context(path.open("rb"))
    magic = raw.read(4)
    raw.seek(0)
    if magic != _ZSTD_MAGIC:
        return raw, ()
    return stack.enter_context(_zstd_file(raw, "rb")), _zstd_errors()


class _Stream:
    def __init__(
        self, source: BufferedIOBase, name: str, errors: tuple[type[Exception], ...]
    ) -> None:
        self._source = source
        self._name = name
        self._errors = errors
        self._data = b""
        self._pos = 0
        self.offset = 0

    def fail(self, offset: int, expected: str) -> TraceFileError:
        return TraceFileError(f"{self._name}: offset {offset}: expected {expected}")

    def _read(self) -> bytes:
        try:
            return self._source.read(_CHUNK)
        except self._errors as error:
            raise self.fail(self.offset, "a complete zstd stream") from error

    def at_end(self) -> bool:
        if self._pos < len(self._data):
            return False
        self._data = self._read()
        self._pos = 0
        return not self._data

    def take(self, size: int, what: str) -> bytes:
        while len(self._data) - self._pos < size:
            more = self._read()
            if not more:
                raise self.fail(self.offset, f"{what}, {size} bytes, before the end of the file")
            self._data = self._data[self._pos :] + more
            self._pos = 0
        chunk = self._data[self._pos : self._pos + size]
        self._pos += size
        self.offset += size
        return chunk


def _header(stream: _Stream) -> None:
    head = stream.take(8, "the PCTR header")
    if head[:4] != _MAGIC:
        raise stream.fail(0, "the PCTR magic")
    version, flags = struct.unpack_from("<HH", head, 4)
    if version != _VERSION:
        raise stream.fail(4, "version 1")
    if flags != 0:
        raise stream.fail(6, "flags 0")


def _records(stream: _Stream) -> Iterator[Record]:
    names: dict[int, str] = {}
    time = 0

    def name(aux: int, at: int) -> str | None:
        if aux == _OVERFLOW:
            return None
        if aux not in names:
            raise stream.fail(at, f"string id {aux} defined before its use")
        return names[aux]

    while not stream.at_end():
        at = stream.offset
        kind_byte, flags, aux, dt = _HEADER.unpack(stream.take(8, "a record header"))
        try:
            kind = TraceKind(kind_byte)
        except ValueError:
            raise stream.fail(at, "a known record kind") from None
        if kind is TraceKind.STR:
            text = stream.take(aux, "string bytes")
            padding = stream.take(-aux & 3, "string padding")
            if any(padding):
                raise stream.fail(at + 8 + aux, "zero padding")
            names[dt] = text.decode("utf-8", errors="replace")
            continue
        payload = _PAYLOADS[kind].unpack(stream.take(4 * PAYLOAD_WORDS[kind], "a payload"))
        match kind:
            case TraceKind.TIME:
                time = payload[0] | payload[1] << 32
                continue
            case TraceKind.BLOCK:
                time += dt
                yield Block(time, payload[0], aux)
            case TraceKind.MEM:
                time += dt
                yield Mem(time, payload[0], payload[1], payload[2], flags & 0x7F, flags & 0x80 != 0)
            case TraceKind.MMIO:
                time += dt
                yield Mmio(
                    time,
                    payload[0],
                    payload[1],
                    payload[2],
                    flags & 0x7F,
                    flags & 0x80 != 0,
                    name(aux, at),
                )
            case TraceKind.IRQ:
                time += dt
                yield Irq(time, aux, flags & 1 != 0)
            case TraceKind.CALL:
                time += dt
                yield Call(time, payload[0], payload[1], name(aux, at))
            case TraceKind.STOP:
                time += dt
                yield Stop(time, payload[0], aux)


def _body(stream: _Stream, stack: ExitStack) -> Iterator[Record]:
    with stack:
        yield from _records(stream)


def read(path: PathLike) -> Iterator[Record]:
    """Reads a .pctr file, raw or zstd-compressed, as typed records in order.

    The file is opened and its header checked before this returns, records are read lazily in
    bounded chunks. Times are absolute instruction counts. compression.zstd is imported only for
    a zstd file.

    Raises:
        FileNotFoundError: the file does not exist.
        TraceFileError: the file is malformed, the message gives the byte offset.
    """
    stack = ExitStack()
    try:
        source, errors = _open(Path(path), stack)
        stream = _Stream(source, os.fspath(path), errors)
        _header(stream)
    except BaseException:
        stack.close()
        raise
    return _body(stream, stack)


def compress(src: PathLike, dst: PathLike | None = None) -> Path:
    """Compresses a .pctr file with zstd level 3, which read() detects by its magic.

    Writes a temporary file beside the destination and renames it over the destination.

    Returns:
        The compressed file, dst or src with `.zst` appended.

    Raises:
        ValueError: the destination is the source.
    """
    source = Path(src)
    target = Path(dst) if dst is not None else source.with_name(source.name + ".zst")
    if target.resolve() == source.resolve():
        raise ValueError(f"compress would overwrite its source {source}")
    temporary = target.with_name(target.name + ".tmp")
    try:
        with source.open("rb") as raw, _zstd_file(temporary, "wb", level=3) as out:
            while chunk := raw.read(_CHUNK):
                out.write(chunk)
        temporary.replace(target)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return target
