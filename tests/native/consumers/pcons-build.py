from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
core = context.current_project.get_target("popcorn_core")
test_support = context.current_project.get_target("popcorn-test-support")
toy = str(project_dir / "tests" / "data" / "desc" / "toy.json")

add_doctest(
    "consumers",
    [
        here / name
        for name in (
            "runtime.cpp",
            "arch_module.cpp",
            "image_loader.cpp",
            "python_facade.cpp",
            "peripheral_models.cpp",
            "observability.cpp",
            "object_linker.cpp",
        )
    ],
    [test_support, core],
    env={"POPCORN_TOY_DESC": toy},
)
