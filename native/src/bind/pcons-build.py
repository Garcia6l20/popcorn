from pathlib import Path

here = Path(__file__).resolve().parent

sources = [
    here / "module.cpp",
    here / "machine.cpp",
    here / "run.cpp",
    here / "views.cpp",
    here / "snapshot.cpp",
    here / "hooks.cpp",
    here / "description.cpp",
    here / "image.cpp",
    here / "errors.cpp",
    here / "config.cpp",
    here / "board.cpp",
    here / "model_context.cpp",
    here / "model_trampoline.cpp",
    here / "devices.cpp",
    here / "observability.cpp",
]
