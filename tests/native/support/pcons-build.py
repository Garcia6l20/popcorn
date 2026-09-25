from pathlib import Path

from pcons import context

project = context.current_project
here = Path(__file__).resolve().parent

smoke_support = project.StaticLibrary(
    "popcorn-test-smoke", project.default_environment, sources=[here / "smoke.cpp"]
)
smoke_support.link(project.get_target("popcorn_core"))
