"""popcorn.coverage: .pcov round trips, merging across workers, lcov export."""

from __future__ import annotations

import random
import struct
from pathlib import Path

import pytest
from popcorn import _obs
from popcorn.coverage import (
    BlockHit,
    CodeRange,
    Coverage,
    CoverageFileError,
    CoverageGroup,
    EdgeHit,
    ImageKind,
    ImageRef,
    merge,
    merge_session,
    worker_path,
)

LINES_ELF = Path(__file__).parents[2] / "firmware" / "obs" / "prebuilt" / "lines.elf"
IMAGE = ImageRef(ImageKind.ELF, "/fw.elf", "aa", 0x100, 0x200)


def group(blocks: dict[tuple[int, int], int], edges: dict[tuple[int, int], int]) -> CoverageGroup:
    return CoverageGroup(
        images=(IMAGE,),
        ranges=(CodeRange("f", 0x100, 0x40),),
        blocks=tuple(BlockHit(start, size, hits) for (start, size), hits in sorted(blocks.items())),
        edges=tuple(
            EdgeHit(source, target, hits) for (source, target), hits in sorted(edges.items())
        ),
    )


def worker(seed: int) -> tuple[dict[tuple[int, int], int], dict[tuple[int, int], int]]:
    chosen = random.Random(seed)
    blocks = {(0x100 + 2 * chosen.randrange(16), 4): chosen.randrange(1, 50) for _ in range(6)}
    edges = {(0x100, 0x100 + 2 * chosen.randrange(16)): chosen.randrange(1, 50) for _ in range(4)}
    return blocks, edges


def test_parts_round_trip_through_bytes_and_files(tmp_path: Path) -> None:
    parts = group({(0x100, 4): 3, (0x104, 8): 1}, {(0x100, 0x104): 1})
    coverage = Coverage.from_parts([parts])
    assert coverage.groups == (parts,)
    assert Coverage(coverage.to_bytes()) == coverage
    path = tmp_path / "one.pcov"
    coverage.save(path)
    assert Coverage.load(path) == coverage
    assert [p.name for p in tmp_path.iterdir()] == ["one.pcov"]


def test_eight_workers_merge_to_the_sum_in_any_order(tmp_path: Path) -> None:
    total_blocks: dict[tuple[int, int], int] = {}
    total_edges: dict[tuple[int, int], int] = {}
    for index in range(8):
        blocks, edges = worker(index)
        for key, hits in blocks.items():
            total_blocks[key] = total_blocks.get(key, 0) + hits
        for key, hits in edges.items():
            total_edges[key] = total_edges.get(key, 0) + hits
        halves = [dict(list(blocks.items())[:3]), dict(list(blocks.items())[3:])]
        for half in halves:
            Coverage.from_parts([group(half, edges if half is halves[0] else {})]).merge_into(
                worker_path(tmp_path, f"gw{index}")
            )
    expected = Coverage.from_parts([group(total_blocks, total_edges)])
    lcov = tmp_path / "out" / "cov.info"
    lcov.parent.mkdir()
    with pytest.raises(CoverageFileError, match="collected against"):
        merge_session(tmp_path, lcov)
    files = sorted(tmp_path.glob("*.pcov"))
    assert merge(files) == expected
    assert merge(reversed(files)) == expected
    shuffled = list(files)
    random.Random(7).shuffle(shuffled)
    assert merge(shuffled).to_bytes() == expected.to_bytes()


def test_lcov_of_the_lines_firmware(tmp_path: Path) -> None:
    image = ImageRef.from_elf(LINES_ELF)
    assert image.kind is ImageKind.ELF
    assert Path(image.path).is_absolute()
    coverage = Coverage.from_parts(
        [
            CoverageGroup(
                images=(image,),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(BlockHit(0x0800_0008, 4, 2), BlockHit(0x0800_0014, 4, 1)),
                edges=(EdgeHit(0x0800_0008, 0x0800_0014, 1),),
            )
        ]
    )
    out = tmp_path / "cov.info"
    coverage.export_lcov(out)
    text = out.read_text().splitlines()
    assert text[0] == "TN:"
    assert text[1].startswith("SF:") and text[1].endswith("lines.c")
    assert "DA:9,2" in text
    assert "DA:18,1" in text
    assert "BRDA:9,0,0,1" in text
    coverage.export_lcov(out, branches=False, test_name="t")
    text = out.read_text().splitlines()
    assert text[0] == "TN:t"
    assert not any(line.startswith("BR") for line in text)


def test_a_raw_image_has_no_lcov_record(tmp_path: Path) -> None:
    raw = ImageRef(ImageKind.RAW, "", "rr", 0x100, 0x200)
    coverage = Coverage.from_parts(
        [
            CoverageGroup(
                images=(raw,),
                ranges=(CodeRange("f", 0x100, 0x10),),
                blocks=(BlockHit(0x100, 4, 1),),
                edges=(),
            )
        ]
    )
    out = tmp_path / "cov.info"
    coverage.export_lcov(out)
    assert out.read_text() == ""


def test_errors(tmp_path: Path) -> None:
    data = Coverage.from_parts([group({(0x100, 4): 1}, {})]).to_bytes()
    with pytest.raises(CoverageFileError):
        Coverage(data[:-1])
    broken = tmp_path / "broken.pcov"
    broken.write_bytes(data[:-1])
    with pytest.raises(CoverageFileError, match=r"broken\.pcov: "):
        Coverage.load(broken)
    with pytest.raises(CoverageFileError, match=r"broken\.pcov: "):
        merge([broken])
    with pytest.raises(FileNotFoundError):
        Coverage.load(tmp_path / "missing.pcov")
    with pytest.raises(ValueError, match="at least one"):
        merge([])
    with pytest.raises(ValueError, match="worker id"):
        worker_path(tmp_path, "../x")
    changed = CoverageGroup(
        images=(ImageRef(ImageKind.ELF, str(LINES_ELF.resolve()), "0", 0, 0),),
        ranges=(),
        blocks=(),
        edges=(),
    )
    with pytest.raises(CoverageFileError, match="changed since collect"):
        Coverage.from_parts([changed]).export_lcov(tmp_path / "x.info")


def test_merge_session_writes_one_lcov_and_nothing_in_the_directory(tmp_path: Path) -> None:
    image = ImageRef.from_elf(LINES_ELF)
    session = tmp_path / "session"
    session.mkdir()
    for index, hits in enumerate((1, 2)):
        Coverage.from_parts(
            [
                CoverageGroup(
                    images=(image,),
                    ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                    blocks=(BlockHit(0x0800_0008, 4, hits),),
                    edges=(),
                )
            ]
        ).save(worker_path(session, f"gw{index}"))
    before = sorted(session.iterdir())
    lcov = tmp_path / "cov.info"
    merged = merge_session(session, lcov)
    assert merged.groups[0].blocks == (BlockHit(0x0800_0008, 4, 3),)
    assert "DA:9,3" in lcov.read_text().splitlines()
    assert sorted(session.iterdir()) == before
    assert merge_session(session, lcov) == merged
    with pytest.raises(ValueError, match="at least one"):
        merge_session(tmp_path / "out", lcov)


def test_merge_session_ignores_saves_in_progress(tmp_path: Path) -> None:
    image = ImageRef.from_elf(LINES_ELF)
    coverage = Coverage.from_parts(
        [
            CoverageGroup(
                images=(image,),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(BlockHit(0x0800_0008, 4, 1),),
                edges=(),
            )
        ]
    )
    coverage.save(worker_path(tmp_path, "gw0"))
    (tmp_path / "gw1.pcov.tmp.1234").write_bytes(b"half")
    assert merge_session(tmp_path, tmp_path / "session.info") == coverage


def read_drcov(path: Path) -> tuple[list[str], list[tuple[int, int, int]]]:
    data = path.read_bytes()
    lines: list[str] = []
    at = 0
    while not lines or not lines[-1].startswith("BB Table:"):
        end = data.index(b"\n", at)
        lines.append(data[at:end].decode())
        at = end + 1
    rest = data[at:]
    assert len(rest) % 8 == 0
    return lines, [struct.unpack_from("<IHH", rest, offset) for offset in range(0, len(rest), 8)]


def test_drcov_of_the_lines_firmware(tmp_path: Path) -> None:
    image = ImageRef.from_elf(LINES_ELF)
    coverage = Coverage.from_parts(
        [
            CoverageGroup(
                images=(image,),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(
                    BlockHit(0x0800_0008, 4, 2),
                    BlockHit(0x0800_0014, 4, 1),
                    BlockHit(0x2000_0000, 2, 1),
                ),
                edges=(),
            )
        ]
    )
    out = tmp_path / "cov.drcov"
    assert coverage.export_drcov(out) == 1
    header, entries = read_drcov(out)
    assert header[:4] == [
        "DRCOV VERSION: 2",
        "DRCOV FLAVOR: drcov",
        "Module Table: version 2, count 1",
        "Columns: id, base, end, entry, checksum, timestamp, path",
    ]
    assert header[4] == (
        f" 0, 0x{image.base:016x}, 0x{image.end:016x}, 0x0000000000000000, 0x00000000,"
        f" 0x00000000, {image.path}"
    )
    assert header[5] == "BB Table: 2 bbs"
    assert entries == [(0x0800_0008 - image.base, 4, 0), (0x0800_0014 - image.base, 4, 0)]
    assert out.read_bytes() == _obs.drcov_bytes(coverage.to_bytes())[0]
