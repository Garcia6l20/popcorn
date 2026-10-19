"""Timing script, not collected by pytest: run with `uv run python
tests/python/description/bench_description_build.py`.

Cold build (find_sources -> build_description) over 10 runs: median and p95, plus a one-shot
phase breakdown (SVD expansion, db/rzone/CMSIS read, JSON dump). Then a warm cache_description
hit over 20 runs, then the written file size. Skips with a message instead of failing when this
machine has no ST data, same as the pytest fixtures.
"""

import statistics
import time

from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._cubemx import read_part, read_rzone
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._irqs import read_cmsis_irqs
from popcorn.stm32.description._sources import Sources, find_sources
from popcorn.stm32.description._svd import expand_svd

_PART = "STM32U575QGIx"
_COLD_RUNS = 10
_HIT_RUNS = 20


def _percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    index = round(fraction * (len(ordered) - 1))
    return ordered[index]


def _report(label: str, samples: list[float]) -> None:
    median = statistics.median(samples)
    p95 = _percentile(samples, 0.95)
    print(f"{label}: median {median:.1f} ms, p95 {p95:.1f} ms over {len(samples)} runs")


def _phase_breakdown(sources: Sources) -> None:
    start = time.perf_counter()
    svd = expand_svd(sources.svd.read_bytes(), file=str(sources.svd))
    svd_ms = (time.perf_counter() - start) * 1000

    start = time.perf_counter()
    read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
    assert sources.rzone is not None
    read_rzone(sources.rzone.read_bytes(), file=str(sources.rzone))
    if sources.cmsis_header is not None:
        read_cmsis_irqs(
            sources.cmsis_header.read_text(encoding="utf-8"), file=str(sources.cmsis_header)
        )
    db_ms = (time.perf_counter() - start) * 1000

    print(
        f"phase breakdown, one shot: expand_svd {svd_ms:.1f} ms "
        f"({len(svd.peripherals)} peripherals), db/rzone/cmsis read {db_ms:.1f} ms"
    )


def main() -> int:
    try:
        sources = find_sources(_PART)
    except DescriptionError as exc:
        print(f"skipped: {exc}")
        return 0

    _phase_breakdown(sources)

    build_ms: list[float] = []
    dump_ms: list[float] = []
    text = ""
    for _ in range(_COLD_RUNS):
        start = time.perf_counter()
        description = build_description(sources)
        build_ms.append((time.perf_counter() - start) * 1000)

        start = time.perf_counter()
        text = dump_json(description)
        dump_ms.append((time.perf_counter() - start) * 1000)

    print(f"JSON size: {len(text.encode())} bytes")
    _report("build_description (cold, includes SVD/db/patches/IRQs)", build_ms)
    _report("dump_json", dump_ms)

    hit_ms: list[float] = []
    path = None
    for _ in range(_HIT_RUNS):
        start = time.perf_counter()
        path = cached_description(_PART)
        hit_ms.append((time.perf_counter() - start) * 1000)
    _report("cache hit", hit_ms)
    if path is not None:
        print(f"cache file size: {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
