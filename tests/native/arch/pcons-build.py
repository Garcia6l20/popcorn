from pathlib import Path

from pcons import context

from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
core = context.current_project.get_target("popcorn_core")

add_doctest("arch-interface", [here / "test_interface.cpp"], [core])
