"""popcorn.trace: the typed .pctr reader, zstd detection and the Chrome JSON converter."""

from __future__ import annotations

import json
import random
import struct
import subprocess
import sys
from pathlib import Path

import pytest
from popcorn import _obs
from popcorn.trace import (
    ALL,
    Block,
    Call,
    Irq,
    Mem,
    Mmio,
    Record,
    Stop,
    TraceFileError,
    TraceKind,
    _reader,
    compress,
    read,
    to_chrome,
)
from popcorn.trace._records import PAYLOAD_WORDS

type Row = tuple[int, int, int, int, int, int, int]

ORIGIN = 1_000
REGISTERS = [f"P{index}.R{index}" for index in range(300)]
SYMBOLS = [f"function_{index}" for index in range(50)]


def synthetic(count: int, seed: int = 1) -> tuple[list[Row], list[Record]]:
    """Records in I08's mix and the typed records a reader must return for them."""
    chosen = random.Random(seed)
    rows: list[Row] = []
    expected: list[Record] = []
    time = ORIGIN
    open_irqs = 0
    for _ in range(count):
        if chosen.random() < 0.001:
            time += 1 << 33
            rows.append((TraceKind.TIME, 0, 0, 0, time & 0xFFFF_FFFF, time >> 32, 0))
        dt = chosen.randrange(1, 40)
        time += dt
        pc = 0x0800_0000 + 2 * chosen.randrange(1 << 16)
        pick = chosen.random()
        if pick < 0.80:
            icount = chosen.randrange(1, 20)
            rows.append((TraceKind.BLOCK, 0, icount, dt, pc, 0, 0))
            expected.append(Block(time, pc, icount))
        elif pick < 0.95:
            address = 0x2000_0000 + 4 * chosen.randrange(1 << 14)
            value = chosen.randrange(1 << 32)
            size = chosen.choice((1, 2, 4))
            write = chosen.random() < 0.5
            rows.append((TraceKind.MEM, size | (0x80 if write else 0), 0, dt, pc, address, value))
            expected.append(Mem(time, pc, address, value, size, write))
        elif pick < 0.99:
            register = chosen.randrange(len(REGISTERS))
            address = 0x4000_0000 + 4 * register
            value = chosen.randrange(1 << 32)
            write = chosen.random() < 0.5
            rows.append(
                (TraceKind.MMIO, 4 | (0x80 if write else 0), register, dt, pc, address, value)
            )
            expected.append(Mmio(time, pc, address, value, 4, write, REGISTERS[register]))
        elif pick < 0.995:
            enter = open_irqs == 0 or chosen.random() < 0.5
            open_irqs += 1 if enter else -1
            rows.append((TraceKind.IRQ, 1 if enter else 0, 16, dt, 0, 0, 0))
            expected.append(Irq(time, 16, enter))
        else:
            symbol = chosen.randrange(len(SYMBOLS))
            r0 = chosen.randrange(1 << 32)
            rows.append((TraceKind.CALL, 0, symbol, dt, pc, r0, 0))
            expected.append(Call(time, pc, r0, SYMBOLS[symbol]))
    rows.append((TraceKind.STOP, 0, 9, 1, 0x0800_0100, 0, 0))
    expected.append(Stop(time + 1, 0x0800_0100, 9))
    return rows, expected


def write(path: Path, rows: list[Row]) -> int:
    return _obs.write_pctr(str(path), ORIGIN, rows, REGISTERS, SYMBOLS)


def test_kinds_match_the_native_enum() -> None:
    native = _obs.pctr_kinds()
    assert [(kind.name.lower(), kind.value, PAYLOAD_WORDS[kind]) for kind in TraceKind] == native
    assert frozenset(TraceKind) - {TraceKind.STR, TraceKind.TIME} == ALL


def test_round_trip_and_size(tmp_path: Path) -> None:
    rows, expected = synthetic(10_000)
    path = tmp_path / "t.pctr"
    size = write(path, rows)
    assert path.stat().st_size == size
    assert list(read(path)) == expected
    assert size / len(expected) <= 16.0


def test_compressed_files_read_the_same(tmp_path: Path) -> None:
    rows, expected = synthetic(2_000, seed=3)
    path = tmp_path / "t.pctr"
    write(path, rows)
    packed = compress(path)
    assert packed == tmp_path / "t.pctr.zst"
    assert packed.read_bytes()[:4] == b"\x28\xb5\x2f\xfd"
    assert list(read(packed)) == expected
    other = compress(path, tmp_path / "other.zst")
    assert list(read(other)) == expected


def test_malformed_files(tmp_path: Path) -> None:
    rows, _ = synthetic(100)
    path = tmp_path / "t.pctr"
    write(path, rows)
    data = path.read_bytes()
    cut = tmp_path / "cut.pctr"
    cut.write_bytes(data[:-3])
    with pytest.raises(TraceFileError, match="offset"):
        list(read(cut))
    magic = tmp_path / "magic.pctr"
    magic.write_bytes(b"XCTR" + data[4:])
    with pytest.raises(TraceFileError, match="offset 0: expected the PCTR magic"):
        list(read(magic))
    unknown = tmp_path / "unknown.pctr"
    unknown.write_bytes(data[:8] + bytes([9]) + data[9:])
    with pytest.raises(TraceFileError, match="offset 8: expected a known record kind"):
        list(read(unknown))
    with pytest.raises(FileNotFoundError):
        list(read(tmp_path / "missing.pctr"))


def run_python(code: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=False)


def test_zstd_is_imported_only_for_zstd_files(tmp_path: Path) -> None:
    rows, _ = synthetic(50)
    path = tmp_path / "t.pctr"
    write(path, rows)
    packed = compress(path)
    raw = run_python(
        "import sys\n"
        "sys.modules['compression.zstd'] = None\n"
        "from popcorn.trace import read\n"
        f"print(len(list(read({str(path)!r}))))\n"
    )
    assert raw.stdout.strip() == str(len(rows)), raw.stderr
    blocked = run_python(
        "import sys\n"
        "sys.modules['compression.zstd'] = None\n"
        "from popcorn.trace import TraceFileError, read\n"
        "try:\n"
        f"    list(read({str(packed)!r}))\n"
        "except TraceFileError as error:\n"
        "    print(error)\n"
    )
    assert "needs Python's compression.zstd" in blocked.stdout, blocked.stderr


def test_chrome_json(tmp_path: Path) -> None:
    rows, _ = synthetic(3_000, seed=5)
    rows.insert(0, (TraceKind.IRQ, 0, 3, 1, 0, 0, 0))
    path = tmp_path / "t.pctr"
    write(path, rows)
    expected = list(read(path))
    assert expected[0] == Irq(ORIGIN + 1, 3, False)
    out = tmp_path / "t.json"
    to_chrome(path, out)
    document = json.loads(out.read_text())
    events: list[dict[str, object]] = document["traceEvents"]
    assert document["otherData"] == {"clock": "instructions"}
    metadata = [event for event in events if event["ph"] == "M"]
    assert len(metadata) == 7
    body = [event for event in events if event["ph"] != "M"]
    still_open = 0
    for record in expected[1:]:
        if isinstance(record, Irq):
            still_open += 1 if record.enter else -1
    assert len(body) == len(expected) - 1 + still_open
    assert "displayTimeUnit" not in document
    depth: dict[object, int] = {}
    for event in body:
        if event["ph"] == "B":
            depth[event["name"]] = depth.get(event["name"], 0) + 1
        if event["ph"] == "E":
            depth[event["name"]] -= 1
            assert depth[event["name"]] >= 0
    assert all(value == 0 for value in depth.values())
    closing = [event for event in body if event["ph"] == "E"][-still_open:] if still_open else []
    assert all(event["ts"] == expected[-1].time for event in closing)
    first_block = next(event for event in body if event["ph"] == "X")
    block = next(record for record in expected if isinstance(record, Block))
    assert first_block["ts"] == block.time - block.icount
    assert first_block["dur"] == block.icount

    only = tmp_path / "mmio.json"
    to_chrome(path, only, kinds={TraceKind.MMIO})
    mmio = [event for event in json.loads(only.read_text())["traceEvents"] if event["ph"] != "M"]
    assert len(mmio) == sum(isinstance(record, Mmio) for record in expected)
    assert all(event["tid"] == 3 for event in mmio)


def test_tiny_chunks_read_the_same(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    rows, expected = synthetic(10_000, seed=11)
    path = tmp_path / "t.pctr"
    write(path, rows)
    packed = compress(path)
    monkeypatch.setattr(_reader, "_CHUNK", 7)
    assert list(read(path)) == expected
    assert list(read(packed)) == expected


def test_a_truncated_zstd_stream_is_a_trace_file_error(tmp_path: Path) -> None:
    rows, _ = synthetic(2_000, seed=13)
    path = tmp_path / "t.pctr"
    write(path, rows)
    packed = compress(path).read_bytes()
    half = tmp_path / "half.pctr.zst"
    half.write_bytes(packed[: len(packed) // 2])
    with pytest.raises(TraceFileError, match="expected a complete zstd stream"):
        list(read(half))


def test_read_checks_the_header_before_iterating(tmp_path: Path) -> None:
    bad = tmp_path / "bad.pctr"
    bad.write_bytes(b"XCTR\x01\x00\x00\x00")
    with pytest.raises(TraceFileError, match="PCTR magic"):
        read(bad)


def test_compress_never_overwrites_its_source(tmp_path: Path) -> None:
    rows, _ = synthetic(10)
    path = tmp_path / "t.pctr"
    write(path, rows)
    before = path.read_bytes()
    with pytest.raises(ValueError, match="overwrite its source"):
        compress(path, path)
    assert path.read_bytes() == before
    compress(path)
    assert sorted(p.name for p in tmp_path.iterdir()) == ["t.pctr", "t.pctr.zst"]


def raw_record(kind: int, flags: int, aux: int, dt: int, payload: bytes = b"") -> bytes:
    return struct.pack("<BBHI", kind, flags, aux, dt) + payload


def test_string_ids_overflow_undefined_and_padding(tmp_path: Path) -> None:
    head = b"PCTR\x01\x00\x00\x00"
    mmio = struct.pack("<3I", 0x0800_0000, 0x4000_0000, 1)
    overflow = tmp_path / "overflow.pctr"
    overflow.write_bytes(head + raw_record(TraceKind.MMIO, 4, 0xFFFF, 5, mmio))
    assert list(read(overflow)) == [Mmio(5, 0x0800_0000, 0x4000_0000, 1, 4, False, None)]
    undefined = tmp_path / "undefined.pctr"
    undefined.write_bytes(head + raw_record(TraceKind.MMIO, 4, 3, 5, mmio))
    with pytest.raises(TraceFileError, match="offset 8: expected string id 3 defined"):
        list(read(undefined))
    padding = tmp_path / "padding.pctr"
    padding.write_bytes(head + raw_record(TraceKind.STR, 0, 1, 0) + b"Ax\x00\x00")
    with pytest.raises(TraceFileError, match="offset 17: expected zero padding"):
        list(read(padding))
