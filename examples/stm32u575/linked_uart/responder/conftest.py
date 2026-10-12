"""The responder alone: the `machine` fixture boots RESPONDER."""

import popcorn
import pytest
from linked_projects import RESPONDER


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return RESPONDER
