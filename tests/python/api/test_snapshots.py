"""Snapshots: restore in place, save, load into another machine, digest checks."""

from __future__ import annotations

import asyncio
import os
import stat
from pathlib import Path

import popcorn
import pytest

from .conftest import DRIVER_ELF, M33_JSON, Machines


def state(m: popcorn.Machine) -> tuple[int, int, tuple[int, ...], bytes]:
    return (
        m.instructions,
        m.time.ps,
        tuple(m.reg[r] for r in popcorn.arm.Reg if r.value in m.reg.names()),
        m.mem.read(0x2000_0000, 256),
    )


def test_restore_returns_registers_memory_and_count(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(budget=10_000))
    saved = m.snapshot()
    before = state(m)
    for _ in range(2):
        asyncio.run(m.run(budget=1_000))
        m.mem.u32[0x2000_0000] = 0xDEAD
        assert state(m) != before
        m.restore(saved)
        assert state(m) == before


def test_a_restored_run_stops_the_same_way(machines: Machines) -> None:
    m = machines()
    saved = m.snapshot()
    first = asyncio.run(m.run(until="alu_mark"))
    after = state(m)
    m.restore(saved)
    second = asyncio.run(m.run(until="alu_mark"))
    assert second == first
    assert state(m) == after


def write_project(tmp_path: Path, name: str, extra: str = "") -> Path:
    path = tmp_path / name
    path.write_text(
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{DRIVER_ELF}"\n{extra}'
    )
    return path


def test_a_saved_snapshot_loads_into_a_second_machine(tmp_path: Path) -> None:
    project = popcorn.Project.load(write_project(tmp_path, "a.toml"))
    first = project.machine()
    first.reset()
    asyncio.run(first.run(budget=5_000))
    snapshot = first.snapshot()
    snapshot.save(tmp_path / "boot.snap")
    assert not list(tmp_path.glob("boot.snap.tmp.*"))

    second = project.machine()
    loaded = second.load_snapshot(tmp_path / "boot.snap")
    assert loaded.machine_id == 0
    assert loaded.config_digest == project.digest
    assert state(second) == state(first)


def test_a_changed_image_or_config_is_refused_naming_the_digest(tmp_path: Path) -> None:
    project = popcorn.Project.load(write_project(tmp_path, "a.toml"))
    m = project.machine()
    m.reset()
    m.snapshot().save(tmp_path / "boot.snap")

    other_image = popcorn.Machine(
        M33_JSON, image=popcorn.Image.elf(DRIVER_ELF.with_name("views.elf"))
    )
    with pytest.raises(popcorn.ConfigError) as image:
        other_image.load_snapshot(tmp_path / "boot.snap")
    assert image.value.key == "image_digests"

    changed = popcorn.Project.load(write_project(tmp_path, "b.toml", "[clock]\ncpi = 2.0\n"))
    with pytest.raises(popcorn.ConfigError) as config:
        changed.machine().load_snapshot(tmp_path / "boot.snap")
    assert config.value.key == "config_digest"


def test_restoring_another_machines_snapshot_is_refused(machines: Machines) -> None:
    first = machines()
    second = machines()
    with pytest.raises(popcorn.ConfigError, match="load its saved form"):
        second.restore(first.snapshot())


def test_malformed_saved_forms_name_the_key_and_the_file(
    machines: Machines, tmp_path: Path
) -> None:
    good = machines().snapshot().to_bytes()
    truncated = tmp_path / "truncated.snap"
    truncated.write_bytes(good[:-10])
    with pytest.raises(popcorn.ConfigError) as short:
        popcorn.Snapshot.load(truncated)
    assert (short.value.key, short.value.file) == ("bytes", str(truncated))

    marker = b"\x12format\x00"
    at = good.index(marker) + len(marker)
    old = tmp_path / "old.snap"
    old.write_bytes(good[:at] + (1).to_bytes(8, "little") + good[at + 8 :])
    with pytest.raises(popcorn.ConfigError) as version:
        popcorn.Snapshot.load(old)
    assert (version.value.key, version.value.file) == ("format", str(old))
    assert isinstance(version.value.__cause__, popcorn.ConfigError)


def test_save_writes_the_whole_file_with_umask_permissions(
    machines: Machines, tmp_path: Path
) -> None:
    snapshot = machines().snapshot()
    path = tmp_path / "boot.snap"
    snapshot.save(path)
    assert path.read_bytes() == snapshot.to_bytes()
    umask = os.umask(0)
    os.umask(umask)
    assert stat.S_IMODE(path.stat().st_mode) == 0o666 & ~umask
    assert popcorn.Snapshot.load(path).image_digests == snapshot.image_digests


def test_a_failed_save_leaves_no_temporary_file(
    machines: Machines, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    snapshot = machines().snapshot()

    def refuse(_: int) -> None:
        raise OSError("disk full")

    monkeypatch.setattr(os, "fsync", refuse)
    with pytest.raises(OSError, match="disk full"):
        snapshot.save(tmp_path / "boot.snap")
    assert list(tmp_path.iterdir()) == []


def test_snapshot_refuses_a_running_machine_even_from_a_handler(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    failures: list[Exception] = []

    def predicate(machine: popcorn.Machine) -> bool:
        try:
            machine.snapshot()
        except popcorn.MachineRunning as error:
            failures.append(error)
        return True

    asyncio.run(m.run(stop_when=predicate))
    assert failures

    async def scenario() -> None:
        task = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning):
            m.snapshot()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
