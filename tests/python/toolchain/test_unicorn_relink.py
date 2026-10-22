"""A rebuilt Unicorn archive relinks the core library and the programs linking the static core."""

from __future__ import annotations

from pathlib import Path

import pytest

ARCHIVE = "deps/install/lib/libunicorn.a"


def edge_inputs(ninja: Path, output: str) -> list[str]:
    edge = next(
        (line for line in ninja.read_text().splitlines() if line.startswith(f"build {output}:")),
        None,
    )
    if edge is None:
        pytest.skip(f"no {output} edge in {ninja}")
    return edge.split()[3:]


@pytest.mark.parametrize(
    "output", ["popcorn/libpopcorn_core.so", "tests/native/core/test-core-run-loop"]
)
def test_the_link_edge_reads_the_unicorn_archive(output: str) -> None:
    ninja = Path(__file__).resolve().parents[3] / "build" / "build.ninja"
    if not ninja.is_file():
        pytest.skip("no build/build.ninja")
    assert ARCHIVE in edge_inputs(ninja, output)
