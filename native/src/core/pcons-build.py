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
        "engine.cpp",
        "hooks.cpp",
        "intercepts.cpp",
        "machine.cpp",
        "memory_map.cpp",
        "mmio.cpp",
        "peripherals.cpp",
        "run_loop.cpp",
        "views.cpp",
        "machine_stub.cpp",
    )
]
