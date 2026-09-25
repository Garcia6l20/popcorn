from pathlib import Path

from pcons import Target, context

from popcorn_build.python import add_extension, add_stub

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")

sources = [
    here / "thumb.cpp",
    here / "ranges.cpp",
    here / "tb.cpp",
    here / "collect.cpp",
    here / "lcov.cpp",
    here / "pcov.cpp",
    here / "refs.cpp",
    here / "drcov.cpp",
    here / "pctr.cpp",
    here / "pctr_machine.cpp",
]
popcorn_obs = project.StaticLibrary("popcorn_obs", project.default_environment, sources=sources)
popcorn_obs.link(core)


def add_python(after: list[Target]) -> list[Target]:
    """The popcorn._obs extension and its stub, the stub generated after @p after."""
    ext = add_extension("popcorn", "_obs", [here / "bind.cpp"])
    ext.link(popcorn_obs)
    return [ext, add_stub(ext, "popcorn", "_obs", after=after)]
