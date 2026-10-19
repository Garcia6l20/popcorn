"""Snapshots: restore in place, save, load into another machine, digest checks."""

from __future__ import annotations

import asyncio
import json
import lzma
import os
import stat
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.units import VirtualDuration, ms, us

from .conftest import DRIVER_ELF, M33_JSON, ROOT, Machines
from .snapshot_doc import bson

USART_ELF = ROOT / "examples" / "stm32u575" / "usart" / "prebuilt" / "usart_echo.elf"
USART_RX_V4 = ROOT / "tests" / "data" / "api" / "usart_rx_v4.snap.xz"


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


def make_project(cpi: float = 1.0) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        part="POPCORN-M33-TEST", description=M33_JSON, images=[popcorn.Elf(DRIVER_ELF)], cpi=cpi
    )


def test_a_saved_snapshot_loads_into_a_second_machine(tmp_path: Path) -> None:
    project = make_project()
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
    project = make_project()
    m = project.machine()
    m.reset()
    m.snapshot().save(tmp_path / "boot.snap")

    other_image = popcorn.Machine(
        M33_JSON, image=popcorn.Image.elf(DRIVER_ELF.with_name("views.elf"))
    )
    with pytest.raises(popcorn.ConfigError) as image:
        other_image.load_snapshot(tmp_path / "boot.snap")
    assert image.value.key == "image_digests"

    changed = make_project(cpi=2.0)
    with pytest.raises(popcorn.ConfigError) as config:
        changed.machine().load_snapshot(tmp_path / "boot.snap")
    assert config.value.key == "config_digest"


def test_a_snapshot_loads_across_provenance_but_not_across_content(tmp_path: Path) -> None:
    raw = json.loads(M33_JSON.read_text())

    def machine(name: str, **changes: object) -> popcorn.Machine:
        path = tmp_path / name
        path.write_text(json.dumps(raw | changes))
        return popcorn.Machine(path, image=popcorn.Image.elf(DRIVER_ELF))

    first = machine("a.json", generator="a", sources={"svd": "0" * 64})
    first.reset()
    asyncio.run(first.run(budget=5_000))
    first.snapshot().save(tmp_path / "a.snap")

    second = machine("b.json", generator="b", sources={"svd": "1" * 64})
    second.load_snapshot(tmp_path / "a.snap")
    assert state(second) == state(first)

    regions = [dict(region) for region in raw["regions"]]
    regions[-1]["size"] += 0x400
    resized = machine("c.json", generator="a", sources={"svd": "0" * 64}, regions=regions)
    with pytest.raises(popcorn.ConfigError) as refused:
        resized.load_snapshot(tmp_path / "a.snap")
    assert refused.value.key == "description_digest"


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


def usart_at(u575: popcorn.PartDescription, after: VirtualDuration) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(USART_ELF))
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    asyncio.run(m.run(for_=after))
    return m


def without_generation(m: popcorn.Machine) -> tuple[int, dict[str, object]]:
    doc = bson(m.snapshot().to_bytes())
    generation = doc.pop("generation")
    assert isinstance(generation, int)
    return generation, doc


def test_a_restore_after_a_reset_runs_as_a_machine_never_reset(
    u575: popcorn.PartDescription,
) -> None:
    m = usart_at(u575, ms(1))
    twin = usart_at(u575, ms(1))
    boot = m.snapshot()
    asyncio.run(m.run(for_=ms(25)))
    m.reset()
    asyncio.run(m.run(until="main", budget=1_000_000))
    m.restore(boot)
    assert without_generation(m)[1] == without_generation(twin)[1]
    for _ in range(5):
        asyncio.run(m.run(for_=ms(5)))
        asyncio.run(twin.run(for_=ms(5)))
        assert m.pending_events == twin.pending_events == 1
        assert without_generation(m)[1] == without_generation(twin)[1]


def test_a_restore_of_a_machines_own_snapshot_adds_no_state(
    u575: popcorn.PartDescription,
) -> None:
    m = usart_at(u575, ms(25))
    twin = usart_at(u575, ms(25))
    generation, before = without_generation(m)
    assert m.pending_events == 1
    m.restore(m.snapshot())
    restored_generation, after = without_generation(m)
    assert restored_generation == generation + 1
    assert after == before
    assert m.pending_events == 1
    for _ in range(3):
        asyncio.run(m.run(for_=ms(5)))
        asyncio.run(twin.run(for_=ms(5)))
        assert without_generation(m)[1] == without_generation(twin)[1]


def test_a_call_adds_no_state_but_the_instructions_it_ran(
    u575: popcorn.PartDescription,
) -> None:
    m = usart_at(u575, ms(25))
    twin = usart_at(u575, ms(25))
    assert m.pending_events == 1
    asyncio.run(m.call("HAL_GetTick"))
    assert m.pending_events == 1
    ran = m.instructions - twin.instructions
    assert ran > 0
    for _ in range(3):
        called = bson(m.snapshot().to_bytes())
        alone = bson(twin.snapshot().to_bytes())
        for doc in (called, alone):
            clock = doc["clock"]
            assert isinstance(clock, dict)
            now = cast(dict[str, object], clock).pop("now")
            assert isinstance(now, dict)
            doc["now"] = cast(dict[str, int], now)["value"]
        assert called.pop("now") == cast(int, alone.pop("now")) + ran
        assert called == alone
        asyncio.run(m.run(for_=ms(5)))
        asyncio.run(twin.run(for_=ms(5)))


def test_a_format_4_snapshot_mid_reception_restores_and_runs_on(
    u575: popcorn.PartDescription,
) -> None:
    """The fixture is the lzma snapshot a format 4 build takes after this case's first half, up to
    the 5 ms + 40 us run. It binds the U575 description digest: regenerate it when that changes."""
    m = usart_at(u575, ms(0))
    port = m.periph.USART1.port
    start = m.time
    port.write(b"alpha\r\n", at=start + ms(5))
    asyncio.run(m.run(for_=ms(5) + us(40)))
    saved = lzma.decompress(USART_RX_V4.read_bytes())
    assert bson(saved)["format"] == 4
    assert m.time.ps == 5_341_000_000
    asyncio.run(port.read_frames(port.available))
    asyncio.run(m.run(for_=ms(5)))
    sent = asyncio.run(port.read_frames(port.available))

    other = usart_at(u575, ms(0))
    other.restore(popcorn.Snapshot.from_bytes(saved))
    asyncio.run(other.run(for_=ms(5)))
    other_port = other.periph.USART1.port
    got = asyncio.run(other_port.read_frames(other_port.available))
    assert [(f.data, f.at) for f in got] == [(f.data, f.at) for f in sent]
    assert b"echo: alpha" in bytes(f.data for f in got)
    assert without_generation(other)[1] == without_generation(m)[1]
