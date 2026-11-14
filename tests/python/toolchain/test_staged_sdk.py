"""The stage-python alias stages the SDK headers next to the Python package.

The cases read the default build tree, which holds the `stage-python` alias in every variant
configured there."""

from __future__ import annotations

from pathlib import Path

import pytest

SDK_FILES = [
    "popcorn/sdk/include/popcorn/core/errors.hpp",
    "popcorn/sdk/include/popcorn/plugin/build_info.hpp",
    "popcorn/sdk/sdk.json",
]


def alias_inputs(alias: str) -> list[str]:
    ninja = Path(__file__).resolve().parents[3] / "build" / "build.ninja"
    if not ninja.is_file():
        pytest.skip("no build/build.ninja")
    edge = next(
        (line for line in ninja.read_text().splitlines() if line.startswith(f"build {alias}:")),
        None,
    )
    if edge is None:
        pytest.fail(f"no {alias} edge in {ninja}")
    return edge.split()[3:]


@pytest.mark.parametrize("staged", SDK_FILES)
def test_stage_python_depends_on_the_staged_sdk(staged: str) -> None:
    assert staged in alias_inputs("stage-python")


def test_stage_python_keeps_the_python_sources() -> None:
    assert "popcorn/__init__.py" in alias_inputs("stage-python")
