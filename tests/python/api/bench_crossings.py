"""The cost of one guest MMIO read served by a Python model, I02's 71 ns figure.

Opt-in, `POPCORN_BENCH=1`, on a quiet host. No pass criterion: the number is recorded.
"""

from __future__ import annotations

import asyncio
import os
import time
from collections.abc import Callable, Mapping

import popcorn
import pytest
from popcorn.model import Model

from .conftest import ROOT
from .models_example import Timer

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"
READS = 200_000


def seconds(models: Mapping[str, Callable[[], Model]]) -> float:
    """Wall time of READS guest reads of TIMER.CNT."""
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON), image=popcorn.Image.elf(MODEL_ELF), models=models
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    started = time.perf_counter()
    result = asyncio.run(m.call("read_timer_count_n", READS))
    elapsed = time.perf_counter() - started
    assert result.stop.reason is popcorn.StopReason.RETURNED
    return elapsed


def test_a_python_model_read_costs_this_much() -> None:
    plain = min(seconds({}) for _ in range(3))
    modelled = min(seconds({"TIMER": Timer}) for _ in range(3))
    per_read = (modelled - plain) / READS
    print(
        f"\nMMIO into Python: {READS} reads, plain {plain * 1e3:.1f} ms, "
        f"Python model {modelled * 1e3:.1f} ms, {per_read * 1e9:.0f} ns per read"
    )
    assert per_read > 0
