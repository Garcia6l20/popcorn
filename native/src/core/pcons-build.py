from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / name
    for name in (
        "version.cpp",
        "errors.cpp",
        "description.cpp",
        "time.cpp",
        "services.cpp",
        "trace.cpp",
        "snapshot.cpp",
        "bus.cpp",
        "calls.cpp",
        "coverage.cpp",
        "engine.cpp",
        "hooks.cpp",
        "host.cpp",
        "intercepts.cpp",
        "machine.cpp",
        "memory_map.cpp",
        "mmio.cpp",
        "peripherals.cpp",
        "run_loop.cpp",
        "snapshots.cpp",
        "spin.cpp",
        "symbols.cpp",
        "trace_feed.cpp",
        "views.cpp",
        "window_barrier.cpp",
    )
]
