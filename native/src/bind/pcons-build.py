from pathlib import Path

here = Path(__file__).resolve().parent

sources = [
    here / "module.cpp",
    here / "machine.cpp",
    here / "run.cpp",
    here / "views.cpp",
    here / "snapshot.cpp",
    here / "description.cpp",
    here / "image.cpp",
    here / "errors.cpp",
    here / "config.cpp",
]
