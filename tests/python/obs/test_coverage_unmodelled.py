"""Coverage of CPU accesses to unmodelled registers: recording, lines, the sidecar."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any, cast

import popcorn
from popcorn.coverage import (
    Coverage,
    UnmodelledAccess,
    UnmodelledReason,
    merge_session,
    unmodelled_path,
)
from popcorn.model import Model, on_read

ROOT = Path(__file__).parents[3]
M33_JSON = ROOT / "tests" / "data" / "core" / "m33.json"
ELF = ROOT / "tests" / "firmware" / "obs" / "prebuilt" / "unmodelled.elf"
SOURCE = ROOT / "tests" / "firmware" / "obs" / "unmodelled.c"
LINES = SOURCE.read_text().splitlines()


class ServedDevice(Model):
    """Serves TESTDEV.RO, every other register of the window is left to the layer."""

    @on_read("RO")
    def _ro(self) -> int:
        return 7


def line_of(text: str) -> str:
    (found,) = [number for number, line in enumerate(LINES, 1) if text in line]
    return str(found)


def machine() -> popcorn.Machine:
    m = popcorn.Machine(
        popcorn.load_description(M33_JSON),
        image=popcorn.Image.elf(ELF),
        models={"TESTDEV": ServedDevice},
    )
    m.reset()
    return m


def drive(m: popcorn.Machine) -> None:
    asyncio.run(m.call("poll_unmodelled", 3))
    asyncio.run(m.call("touch_stat", 5))
    asyncio.run(m.call("read_unimplemented"))
    asyncio.run(m.call("read_no_register"))
    asyncio.run(m.call("read_modelled"))


def recorded(
    m: popcorn.Machine,
) -> dict[tuple[str, str, bool, UnmodelledReason], int]:
    (group,) = m.coverage.collect().groups
    sums: dict[tuple[str, str, bool, UnmodelledReason], int] = {}
    for u in group.unmodelled:
        key = (u.peripheral, u.register, u.write, u.reason)
        sums[key] = sums.get(key, 0) + u.hits
    return sums


def sidecar(coverage: Coverage, path: Path) -> dict[str, dict[str, list[dict[str, Any]]]]:
    coverage.export_unmodelled(path)
    document = json.loads(path.read_text())
    assert document["version"] == 1
    return cast(dict[str, dict[str, list[dict[str, Any]]]], document["files"])


def test_the_recording_is_opt_in() -> None:
    m = machine()
    m.coverage.enable()
    drive(m)
    assert m.coverage.collect().groups[0].unmodelled == ()


def test_cpu_accesses_to_unmodelled_registers_are_counted_and_served_ones_are_not() -> None:
    m = machine()
    m.coverage.enable(unmodelled=True)
    drive(m)
    assert recorded(m) == {
        ("UNMODELLED", "DATA", False, UnmodelledReason.NO_MODEL): 3,
        ("UNMODELLED", "STAT", True, UnmodelledReason.NO_MODEL): 1,
        ("UNMODELLED", "STAT", False, UnmodelledReason.NO_MODEL): 2,
        ("TESTDEV", "RW", False, UnmodelledReason.UNIMPLEMENTED): 1,
        ("TESTDEV", "", False, UnmodelledReason.NO_REGISTER): 1,
    }
    m.coverage.disable()
    drive(m)
    assert sum(recorded(m).values()) == 8


def test_accesses_are_summed_per_line_and_ranges_limit_the_lines(tmp_path: Path) -> None:
    m = machine()
    m.coverage.enable(functions=["poll_unmodelled", "touch_stat"], unmodelled=True)
    drive(m)
    coverage = m.coverage.collect()
    assert len({u.pc for u in coverage.groups[0].unmodelled}) == 6
    files = sidecar(coverage, tmp_path / "u.json")
    (lines,) = files.values()

    def use(peripheral: str, register: str, write: bool, hits: int) -> dict[str, Any]:
        offset = {"DATA": 0, "STAT": 4}[register]
        return {
            "peripheral": peripheral,
            "register": register,
            "offset": offset,
            "write": write,
            "reason": "no_model",
            "hits": hits,
        }

    assert lines == {
        line_of("sum += UNMODELLED_DATA"): [use("UNMODELLED", "DATA", False, 3)],
        line_of("UNMODELLED_STAT = value"): [use("UNMODELLED", "STAT", True, 1)],
        line_of("sink            = UNMODELLED_STAT"): [use("UNMODELLED", "STAT", False, 2)],
    }
    assert next(iter(files)).endswith("unmodelled.c")


def test_every_listed_line_has_a_da_record_in_the_lcov_file(tmp_path: Path) -> None:
    m = machine()
    m.coverage.enable(unmodelled=True)
    drive(m)
    coverage = m.coverage.collect()
    lcov = tmp_path / "cov.info"
    coverage.export_lcov(lcov)
    data = {
        line.split(":")[1].split(",")[0]
        for line in lcov.read_text().splitlines()
        if line[:3] == "DA:"
    }
    files = sidecar(coverage, tmp_path / "u.json")
    (lines,) = files.values()
    assert set(lines) == {
        line_of("sum += UNMODELLED_DATA"),
        line_of("UNMODELLED_STAT = value"),
        line_of("sink            = UNMODELLED_STAT"),
        line_of("return TESTDEV_RW"),
        line_of("return TESTDEV_GAP"),
    }
    assert set(lines) <= data
    assert line_of("return TESTDEV_RO") not in lines


def test_a_coverage_without_accesses_has_an_empty_sidecar(tmp_path: Path) -> None:
    m = machine()
    m.coverage.enable()
    drive(m)
    assert sidecar(m.coverage.collect(), tmp_path / "u.json") == {}


def test_merged_workers_sum_their_hits(tmp_path: Path) -> None:
    for worker in ("gw0", "gw1"):
        m = machine()
        m.coverage.enable(unmodelled=True)
        drive(m)
        m.coverage.merge_into(tmp_path / f"{worker}.pcov")
    lcov = tmp_path / "out" / "cov.info"
    lcov.parent.mkdir()
    merged = merge_session(tmp_path, lcov, unmodelled=True)
    assert sum(u.hits for u in merged.groups[0].unmodelled) == 16
    path = unmodelled_path(lcov)
    assert path.name == "cov.info.unmodelled.json"
    (lines,) = json.loads(path.read_text())["files"].values()
    assert lines[line_of("sum += UNMODELLED_DATA")][0]["hits"] == 6
    lcov2 = tmp_path / "out" / "plain.info"
    merge_session(tmp_path, lcov2)
    assert not unmodelled_path(lcov2).exists()


def test_merging_without_the_sidecar_removes_one_of_an_earlier_run(tmp_path: Path) -> None:
    m = machine()
    m.coverage.enable(unmodelled=True)
    drive(m)
    m.coverage.merge_into(tmp_path / "gw0.pcov")
    lcov = tmp_path / "out" / "cov.info"
    lcov.parent.mkdir()
    merge_session(tmp_path, lcov, unmodelled=True)
    assert unmodelled_path(lcov).exists()
    merge_session(tmp_path, lcov)
    assert not unmodelled_path(lcov).exists()


def test_an_access_is_a_dataclass_of_plain_values() -> None:
    access = UnmodelledAccess(1, "P", "R", 4, True, UnmodelledReason.STORAGE_ONLY, 2)
    assert access.reason.name.lower() == "storage_only"
