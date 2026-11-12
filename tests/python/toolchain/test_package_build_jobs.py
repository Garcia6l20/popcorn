import tomllib
from collections.abc import Callable
from importlib import import_module
from pathlib import Path
from typing import Any, cast

PYPROJECT = Path(__file__).resolve().parents[3] / "pyproject.toml"
JOB_CAP = 8


def test_the_package_build_caps_its_ninja_jobs() -> None:
    config = tomllib.loads(PYPROJECT.read_text())["tool"]["pcons"]
    jobs = cast(
        Callable[[dict[str, Any] | None, dict[str, Any]], int | None],
        import_module("pcons.pyproject")._jobs,
    )
    assert jobs(None, config) == JOB_CAP
