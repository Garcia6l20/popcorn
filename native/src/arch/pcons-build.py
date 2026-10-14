from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / "arch.cpp",
    *(
        here / "armv8m" / name
        for name in (
            "cortex_m.cpp",
            "system_space.cpp",
            "exceptions.cpp",
            "timers.cpp",
            "faults.cpp",
            "calls.cpp",
            "sleep.cpp",
            "thumb.cpp",
            "analysis.cpp",
            "factory.cpp",
        )
    ),
]
