from pathlib import Path

from pcons import Target, context

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")

sources = [here / "thumb.cpp", here / "ranges.cpp", here / "tb.cpp", here / "collect.cpp"]
popcorn_obs = project.StaticLibrary("popcorn_obs", project.default_environment, sources=sources)
popcorn_obs.link(core)


def add_python(after: list[Target]) -> list[Target]:
    """The popcorn._obs extension and its stub, none until the extension exists."""
    return []
