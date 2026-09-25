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
        "machine_stub.cpp",
    )
]
