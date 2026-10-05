"""The spec 03 examples, run on the test firmware."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn

from .conftest import DRIVER_ELF, M33_JSON, ROOT, Machines
from .examples import spec03

MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"


def test_section_1_project_and_machine() -> None:
    project = popcorn.Project(
        part="POPCORN-M33-TEST", description=M33_JSON, images=[popcorn.Elf(DRIVER_ELF)]
    )
    assert asyncio.run(spec03.project_and_machine(project)).machine_id != 0
    m = spec03.machine_without_project(popcorn.load_description(M33_JSON), DRIVER_ELF)
    assert m.images[0].path == DRIVER_ELF


def test_section_2_running(machines: Machines) -> None:
    stop = asyncio.run(spec03.running(machines(slice_instructions=50)))
    assert stop.reason is popcorn.StopReason.USER


def test_section_3_calling(callee: popcorn.Machine) -> None:
    assert asyncio.run(spec03.calling(callee)) == (42, 21)


def test_section_4_memory(views: popcorn.Machine) -> None:
    raw, target = spec03.memory(views)
    assert len(raw) == 4 and target == 40


def test_section_5_intercepts(hooked: popcorn.Machine) -> None:
    log = asyncio.run(spec03.intercepts(hooked))
    assert log


def test_section_6_board(board: popcorn.Machine) -> None:
    assert asyncio.run(spec03.board(board)) == b"ping\r\n"


def test_section_7_model() -> None:
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": spec03.Counter},
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    asyncio.run(m.call("start_timer", 10))
    result = asyncio.run(m.call("wait_timer_irqs", 1, interrupts=True, budget=100_000))
    assert result.value == 1


def test_section_8_snapshots(machines: Machines, tmp_path: Path) -> None:
    m = machines()
    assert spec03.snapshots(m, tmp_path / "boot.snap").machine_id == 0


def test_section_9_observability(machines: Machines, tmp_path: Path) -> None:
    assert "stop  BUDGET" in asyncio.run(spec03.observability(machines(), tmp_path))
    assert (tmp_path / "cov.info").read_text().startswith("TN:")
