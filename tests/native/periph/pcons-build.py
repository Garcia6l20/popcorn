from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
core = context.current_project.get_target("popcorn_core")
toy = str(project_dir / "tests" / "data" / "desc" / "toy.json")

add_doctest(
    "periph-interfaces",
    [here / "test_interfaces.cpp", here / "test_lines.cpp", here / "test_register_file.cpp"],
    [core],
    env={"POPCORN_TOY_DESC": toy},
)
