"""The cost of a guest RNG DR read served by a native plugin model and by a Python model, the
figures of the native plugins guide page.

Opt-in, `POPCORN_BENCH=1`, on a quiet host. No pass criterion: the numbers are recorded. The
plugin is the native_plugin example's, the Python model the python_model example's, both on the
python_model firmware.
"""

from __future__ import annotations

import asyncio
import importlib.util
import os
import statistics
import struct
import time
from collections.abc import Callable
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.model import Model
from popcorn.plugin import Plugin
from popcorn.units import ms

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

EXAMPLES = Path(__file__).resolve().parents[3] / "examples" / "stm32u575"
READS = 1_000_000
RUNS = 11
WORDS = 8
LOOP_AT = 0x20020000
LOOP = struct.pack("<4H", 0x6801, 0x3A01, 0xD1FC, 0xE7FE)
"""`ldr r1, [r0]`, `subs r2, #1`, `bne` back to the load, then `b .`."""


def python_rng() -> type[Model]:
    """`rngmodel.Rng` of the python_model example."""
    spec = importlib.util.spec_from_file_location(
        "rngmodel", EXAMPLES / "python_model" / "rngmodel.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return cast(type[Model], module.Rng)


def descriptions() -> dict[str, popcorn.MachineDescription]:
    """The python_model firmware with the Python model, the plugin model, by name."""
    plugin = Plugin.dev(EXAMPLES / "native_plugin" / "plugin")
    base = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(EXAMPLES / "python_model" / "prebuilt" / "rng_numbers.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
    )
    return {
        "python": base.replace(board={"RNG": popcorn.Periph(model=popcorn.Use(python_rng()))}),
        "plugin": base.replace(
            plugins=[plugin], board={"RNG": popcorn.Periph(model=plugin.model("rng"))}
        ),
    }


async def booted(description: popcorn.MachineDescription) -> popcorn.Machine:
    machine = description.machine()
    await description.boot_machine(machine)
    return machine


async def loop_seconds(description: popcorn.MachineDescription, register: str) -> float:
    """Wall time of READS guest reads of RNG `register`, DRDY clear for DR."""
    machine = await booted(description)
    (rng,) = (p for p in description.part_description().peripherals if p.name == "RNG")
    (offset,) = (r.offset for r in rng.registers if r.name == register)
    machine.mem.write(LOOP_AT, LOOP)
    machine.reg.r0 = rng.base + offset
    machine.reg.r2 = READS
    machine.reg.pc = LOOP_AT
    started = time.perf_counter()
    stop = await machine.run(until=LOOP_AT + 6, budget=10 * READS)
    elapsed = time.perf_counter() - started
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    assert machine.reg.r2 == 0
    return elapsed


async def run_seconds(description: popcorn.MachineDescription) -> float:
    """Wall time of the firmware's run from main to its eighth RNG word."""
    machine = await booted(description)
    started = time.perf_counter()
    stop = await machine.run(until="rng_done", for_=ms(20), budget=2_000_000)
    elapsed = time.perf_counter() - started
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    assert machine.var("rng_result").count.value == WORDS
    return elapsed


def medians(cases: dict[str, Callable[[], float]]) -> dict[str, float]:
    """The median of RUNS timings of each case, interleaved, after one warm-up run each."""
    for case in cases.values():
        case()
    timed: dict[str, list[float]] = {name: [] for name in cases}
    for _ in range(RUNS):
        for name, case in cases.items():
            timed[name].append(case())
    return {name: statistics.median(values) for name, values in timed.items()}


def test_a_guest_rng_read_costs_this_much() -> None:
    described = descriptions()
    found = medians(
        {
            "Python model": lambda: asyncio.run(loop_seconds(described["python"], "DR")),
            "plugin model": lambda: asyncio.run(loop_seconds(described["plugin"], "DR")),
            "unclaimed SR": lambda: asyncio.run(loop_seconds(described["plugin"], "SR")),
        }
    )
    print(f"\n{READS} RNG reads, medians of {RUNS} runs, per read:")
    for name, seconds in found.items():
        print(f"  {name}: {seconds / READS * 1e9:.1f} ns")
    assert all(seconds > 0 for seconds in found.values())


def test_a_firmware_run_to_its_eighth_word_takes_this_long() -> None:
    described = descriptions()
    found = medians(
        {
            "Python model": lambda: asyncio.run(run_seconds(described["python"])),
            "plugin model": lambda: asyncio.run(run_seconds(described["plugin"])),
        }
    )
    print(f"\nRun from main to the {WORDS}th RNG word, medians of {RUNS} runs:")
    for name, seconds in found.items():
        print(f"  {name}: {seconds * 1e3:.3f} ms")
    assert all(seconds > 0 for seconds in found.values())
