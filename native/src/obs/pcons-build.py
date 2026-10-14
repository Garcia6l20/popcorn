from pathlib import Path

from pcons import Target

from popcorn_build.python import add_extension, add_stub

here = Path(__file__).resolve().parent

sources = [
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


def add_python(popcorn_obs: Target, after: list[Target]) -> list[Target]:
    """The popcorn._obs extension over @p popcorn_obs and its stub, generated after @p after."""
    ext = add_extension("popcorn", "_obs", [here / "bind.cpp"])
    ext.link(popcorn_obs)
    return [ext, add_stub(ext, "popcorn", "_obs", after=after)]
