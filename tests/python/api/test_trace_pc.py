"""The pc of a Mem record is the address of the load or store, whatever ran before it.

`b7_call` takes SysTick exceptions, and each exception return writes the pc from inside an engine
hook. The run goes on in the same slice, so the engine's pc sync must stay exact for every access
after it. The expected pc of every access comes from an independent run that steps one
instruction at a time and reads the pc register between steps.

The stepped and the sliced runs take SysTick at different instructions, so the handler records
differ in count and order. The sliced run is checked for set membership of
(pc, address, size, write) in the stepped run for that reason: comparing the lists would fail
without any pc being wrong.

A core parked at a WFI has the same raw pc register whether or not an exception return ran
before the WFI.
"""

from __future__ import annotations

import asyncio
import tempfile
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import trace
from popcorn.units import ms

from .conftest import CORE_FIRMWARE, M33_JSON
from .snapshot_doc import bson

K = trace.TraceKind
INSTRUCTIONS = 20_000
KINDS = frozenset({K.MEM, K.IRQ, K.STOP})

Access = tuple[int, int, int, bool]


def machine(**slicing: Any) -> popcorn.Machine:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(CORE_FIRMWARE / "b7_call.elf"), **slicing)
    m.reset()
    return m


def accesses(records: list[Any]) -> list[trace.Mem]:
    return [record for record in records if isinstance(record, trace.Mem)]


def key(record: trace.Mem) -> Access:
    return (record.pc, record.address, record.size, record.write)


def run_in_slices(**slicing: Any) -> tuple[list[Any], int]:
    m = machine(**slicing)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "run.pctr"
        with m.trace(KINDS, path):
            stop = asyncio.run(m.run(budget=INSTRUCTIONS))
        return list(trace.read(path)), stop.instructions


def run_stepped(count: int) -> list[tuple[int, trace.Mem]]:
    m = machine()
    before: list[int] = []

    async def step_all() -> None:
        for _ in range(count):
            before.append(int(m.reg.pc))
            await m.step(1)

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "run.pctr"
        with m.trace(KINDS, path):
            asyncio.run(step_all())
        owner = -1
        expected: list[tuple[int, trace.Mem]] = []
        for record in trace.read(path):
            if isinstance(record, trace.Stop):
                owner += 1
            elif isinstance(record, trace.Mem):
                expected.append((before[owner + 1], record))
        return expected


def test_a_stepped_run_reports_the_instruction_address() -> None:
    stepped = run_stepped(INSTRUCTIONS)
    assert len(stepped) > 1000
    assert all((pc & ~1) == (record.pc & ~1) for pc, record in stepped)


@pytest.mark.parametrize(
    "slicing",
    [{}, {"slice_instructions": 7919, "slice_wall_budget": 1e-7}],
    ids=["default", "odd"],
)
def test_a_sliced_run_reports_the_addresses_a_stepped_run_does(slicing: dict[str, Any]) -> None:
    records, executed = run_in_slices(**slicing)
    returned = next(
        index
        for index, record in enumerate(records)
        if isinstance(record, trace.Irq) and not record.enter
    )
    assert len(accesses(records[returned:])) > 1000
    truth = {key(record) for _, record in run_stepped(executed)}
    assert {key(record) for record in accesses(records)} <= truth


def parked_pc(m: popcorn.Machine, duration_ms: float) -> tuple[int, int]:
    stop = asyncio.run(m.run(for_=ms(duration_ms), budget=10**9))
    registers = cast(dict[str, dict[str, Any]], bson(m.snapshot().to_bytes())["registers"])
    raw = next(r["value"] for r in registers.values() if r["name"] == "pc")
    return int(raw), int(stop.pc)


def test_a_parked_core_has_the_same_raw_pc_after_an_exception_return() -> None:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(CORE_FIRMWARE / "b7_wfi.elf"))
    m.reset()
    first = parked_pc(m, 0.5)
    later = parked_pc(m, 5.5)
    assert first[1] == later[1]
    assert first[0] == later[0]
