"""The controller alone: the `machine` fixture boots CONTROLLER."""

import popcorn
import pytest
from linked_projects import CONTROLLER


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return CONTROLLER
