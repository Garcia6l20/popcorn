from pathlib import Path

from pcons import context

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")

smoke_support = project.StaticLibrary(
    "popcorn-test-smoke", project.default_environment, sources=[here / "smoke.cpp"]
)
smoke_support.link(core)

test_support = project.StaticLibrary(
    "popcorn-test-support",
    project.default_environment,
    sources=[here / "test_arch.cpp", here / "elf_image.cpp", here / "test_model.cpp"],
)
test_support.link(core)
