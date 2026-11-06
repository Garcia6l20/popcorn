"""Points the shipped-composition store at the run directory for every test of this directory."""

from collections.abc import Iterator
from pathlib import Path

import pytest

from .composed import share


@pytest.fixture(scope="session", autouse=True)
def shared_compositions(run_directory: Path) -> Iterator[None]:
    directory = run_directory / "compositions"
    directory.mkdir(exist_ok=True)
    share(directory)
    yield
    share(None)
