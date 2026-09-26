from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.testing import add_doctest

project = context.current_project
here = Path(__file__).resolve().parent

add_doctest(
    "config-project",
    [here / "test_project.cpp"],
    [project.get_target("popcorn_config"), project.get_target("popcorn_core")],
    env={"POPCORN_API_DATA": str(project_dir / "tests" / "data" / "api")},
)
