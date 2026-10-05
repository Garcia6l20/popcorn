from pathlib import Path

import popcorn
import pytest

PROJECT = Path(__file__).parent / "popcorn.toml"


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return popcorn.Project.load(PROJECT).with_configs(
        seeded=popcorn.Project.load(PROJECT, "seeded"),
    )
