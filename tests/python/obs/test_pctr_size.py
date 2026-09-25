"""FR-OBS-4 at full size: 1e6 events written and read back, at most 16 MB. Opt-in."""

from __future__ import annotations

import os
import time
from pathlib import Path

import pytest
from popcorn.trace import read

from .test_trace import synthetic, write

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_OBS_SIZE") != "1", reason="set POPCORN_OBS_SIZE=1 to run it"
)


def test_a_million_events_fit_in_16_mb(tmp_path: Path) -> None:
    rows, expected = synthetic(1_000_000, seed=17)
    path = tmp_path / "million.pctr"
    started = time.perf_counter()
    size = write(path, rows)
    written = time.perf_counter()
    records = list(read(path))
    done = time.perf_counter()
    assert records == expected
    assert size <= 16_000_000
    print(
        f"\nFR-OBS-4: {len(expected)} events, {size} bytes, {size / len(expected):.2f} B/event,"
        f" write {written - started:.2f} s, read {done - written:.2f} s"
    )
