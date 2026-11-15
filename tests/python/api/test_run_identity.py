"""Run identity: what a run records stays the same across engine changes and slicings.

Each subject runs a fixed script of stops under two slicings. The digests of the stop records,
the snapshot at each stop of the run with no sink, the UART frames, the trace stream with block
records off and on, and the coverage must equal the golden in
`tests/data/run_identity/<subject>.json`, recorded from the default slicing. The snapshot
digests leave out the version and Unicorn commit fields.

`POPCORN_UPDATE_GOLDEN=1 pytest -k default` rewrites the goldens, a change that moves one is a
change of observable behaviour. The `-k default` matters: the odd slicing cases assert against
the files the default cases write.

The odd slicing, `slice_instructions=7919` with a 100 ns wall slice, slices at the popcorn
level and does not move the engine's own instruction cap. The runs are millions of instructions
long, so the default slicing already ends many runs on that cap. Short `budget=` steps end
runs on a small instruction budget, and the `b7_hooked` subject runs with an intercept and a
`call`.

The trace digests of runs with memory records leave out the pc of memory and MMIO records:
it depends on the wall-time slicing of the run. The `trace_mmio` and `trace_pc` digests keep
it, over traces with no memory records.
"""

from __future__ import annotations

import asyncio
import dataclasses
import hashlib
import json
import os
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import trace
from popcorn._refs import Pending
from popcorn.units import VirtualTime, ms

from .conftest import CORE_FIRMWARE, M33_JSON, ROOT
from .snapshot_doc import bson

EXAMPLES = ROOT / "examples" / "stm32u575"
IRQ_LL = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "irq_ll.elf"
GOLDEN = ROOT / "tests" / "data" / "run_identity"
UPDATE = os.environ.get("POPCORN_UPDATE_GOLDEN") == "1"
SLICINGS = {
    "default": {},
    "odd": {"slice_instructions": 7919, "slice_wall_budget": 1e-7},
}
BOOT = {"until": "main", "budget": 1_000_000}
LINE = b"alpha\r\n"

Step = dict[str, Any]


def times(*durations: int) -> list[Step]:
    return [{"for_": ms(duration), "budget": 10**9} for duration in durations]


def short(instructions: int) -> Step:
    return {"budget": instructions, "for_": ms(20)}


def call(function: str) -> Step:
    return {"call": function}


def until(symbol: str, within: int) -> Step:
    return {"until": symbol, "for_": ms(within), "budget": 10**9}


def write_line(m: popcorn.Machine) -> list[Pending[VirtualTime]]:
    return [m.periph.USART1.port.write(LINE, at=m.time + ms(5))]


def hook_systick(m: popcorn.Machine) -> list[Pending[VirtualTime]]:
    m.intercept("SysTick_Handler", lambda call: call.proceed())
    return []


def irq_wfi(m: popcorn.Machine) -> list[Pending[VirtualTime]]:
    m.var("irq_period_cycles").value = 1600
    m.var("irq_stores").value = 0
    m.reg.pc = m.sym("irq_wfi")
    return []


@dataclass(frozen=True)
class Subject:
    name: str
    elf: Path
    steps: list[Step]
    part: str = "u575"
    boot: bool = True
    setup: Callable[[popcorn.Machine], list[Pending[VirtualTime]]] = field(
        default=lambda _: [], compare=False
    )


SUBJECTS = [
    Subject(
        name,
        CORE_FIRMWARE / f"{name}.elf",
        [*times(1), short(311), *times(8, 40), until("SysTick_Handler", 5)],
        part="core",
        boot=False,
    )
    for name in ("b7_inline", "b7_call", "b7_bdot", "b7_wfi")
] + [
    Subject(
        "b7_hooked",
        CORE_FIRMWARE / "b7_call.elf",
        [*times(1), short(311), call("HAL_GetTick"), *times(8), until("SysTick_Handler", 5)],
        part="core",
        boot=False,
        setup=hook_systick,
    ),
    Subject(
        "irq_wfi",
        IRQ_LL,
        [*times(1), short(311), *times(8, 40), until("TIM6_IRQHandler", 5)],
        setup=irq_wfi,
    ),
    Subject(
        "tim_ticks",
        EXAMPLES / "tim" / "prebuilt" / "tim_ticks.elf",
        [*times(1), short(311), *times(8, 40), until("TIM2_IRQHandler", 5)],
    ),
    Subject(
        "usart_echo",
        EXAMPLES / "usart" / "prebuilt" / "usart_echo.elf",
        [*times(1), short(311), *times(8, 40), until("HAL_IncTick", 5)],
        setup=write_line,
    ),
    Subject(
        "gpdma_uart",
        EXAMPLES / "gpdma" / "prebuilt" / "gpdma_uart.elf",
        [*times(1), short(311), *times(8), until("all_transfers_done", 100), *times(1)],
    ),
]


BUILD_FIELDS = ("popcorn_version", "unicorn_commit")


def snapshot_digest(m: popcorn.Machine) -> str:
    document = bson(m.snapshot().to_bytes())
    return sha({key: value for key, value in document.items() if key not in BUILD_FIELDS})


def sha(*parts: object) -> str:
    return hashlib.sha256(repr(parts).encode()).hexdigest()[:16]


def trace_digest(path: Path, masked: bool) -> list[Any]:
    digest = hashlib.sha256()
    count = 0
    for record in trace.read(path):
        if masked and isinstance(record, trace.Mem | trace.Mmio):
            record = dataclasses.replace(record, pc=0)
        digest.update(repr(record).encode())
        count += 1
    return [digest.hexdigest()[:16], count]


def coverage_digest(m: popcorn.Machine) -> list[Any]:
    groups = m.coverage.collect().groups
    blocks = [[(b.start, b.size, b.hits) for b in group.blocks] for group in groups]
    edges = [[(e.source, e.target, e.hits) for e in group.edges] for group in groups]
    split = [(group.split, group.dropped) for group in groups]
    return [
        sha(blocks, edges, split),
        sum(len(group) for group in blocks),
        sum(len(group) for group in edges),
    ]


async def drive(
    m: popcorn.Machine, subject: Subject, snapshot: bool
) -> tuple[list[list[Any]], list[str], list[Any]]:
    stops: list[list[Any]] = []
    snapshots: list[str] = []

    async def go(step: Step) -> None:
        if "call" in step:
            result = await m.call(step["call"])
            stops.append(["call", result.value, str(result.stop.reason), result.stop.time.ps])
            return
        stop = await m.run(**step)
        stops.append([str(stop.reason), stop.pc, stop.instructions, stop.time.ps])
        if snapshot:
            snapshots.append(snapshot_digest(m))

    if subject.boot:
        await go(BOOT)
    written = subject.setup(m)
    for step in subject.steps:
        await go(step)
    for pending in written:
        await pending
    frames: list[Any] = []
    if subject.part == "u575":
        port = m.periph.USART1.port
        frames = [(frame.data, frame.at.ps) for frame in await port.read_frames(port.available)]
    return stops, snapshots, frames


K = trace.TraceKind
TRACES: dict[str, tuple[frozenset[trace.TraceKind], bool, bool]] = {
    "bare": (frozenset(), False, False),
    "trace": (trace.ALL - {K.BLOCK}, True, False),
    "trace_blocks": (trace.ALL, True, True),
    "trace_mmio": (frozenset({K.MMIO, K.IRQ, K.CALL, K.STOP}), False, False),
    "trace_pc": (frozenset({K.BLOCK, K.MMIO, K.IRQ, K.CALL, K.STOP}), False, False),
}


def record(
    subject: Subject,
    part: popcorn.PartDescription | Path,
    slicing: dict[str, Any],
    name: str,
) -> dict[str, Any]:
    kinds, masked, covered = TRACES[name]
    m = popcorn.Machine(part, image=popcorn.Image.elf(subject.elf), **slicing)
    m.reset()
    if covered:
        m.coverage.enable()
    seen: list[Any] = []
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "run.pctr"
        if kinds:
            with m.trace(kinds, path):
                stops, snapshots, frames = asyncio.run(drive(m, subject, name == "bare"))
            seen = trace_digest(path, masked)
        else:
            stops, snapshots, frames = asyncio.run(drive(m, subject, name == "bare"))
    out: dict[str, Any] = {
        "stops": stops,
        "snapshots": sha(snapshots) if snapshots else "",
        "uart": sha(frames) if frames else "",
    }
    if kinds:
        out[name] = seen
    if covered:
        out["coverage"] = coverage_digest(m)
    return out


def observe(
    subject: Subject, part: popcorn.PartDescription | Path, slicing: dict[str, Any]
) -> dict[str, Any]:
    first, *others = (record(subject, part, slicing, name) for name in TRACES)
    for other in others:
        for key in ("stops", "uart"):
            assert other.pop(key) == first[key]
        other.pop("snapshots")
        first.update(other)
    return first


@pytest.mark.parametrize("slicing", SLICINGS)
@pytest.mark.parametrize("subject", SUBJECTS, ids=lambda subject: subject.name)
def test_run_records_the_golden(
    subject: Subject, slicing: str, request: pytest.FixtureRequest
) -> None:
    part = M33_JSON if subject.part == "core" else request.getfixturevalue("u575")
    golden = GOLDEN / f"{subject.name}.json"
    if UPDATE and slicing == "default":
        golden.parent.mkdir(exist_ok=True)
        golden.write_text(json.dumps(observe(subject, part, SLICINGS[slicing]), indent=1) + "\n")
    assert observe(subject, part, SLICINGS[slicing]) == json.loads(golden.read_text())


def test_every_golden_has_a_subject() -> None:
    assert sorted(path.stem for path in GOLDEN.glob("*.json")) == sorted(
        subject.name for subject in SUBJECTS
    )
