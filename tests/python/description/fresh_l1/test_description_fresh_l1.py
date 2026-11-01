"""The L1 shipped descriptions against a fresh ship build."""

import pytest

from ..fresh_common import L1_PREFIX, check_fresh, names_of


@pytest.mark.parametrize("name", names_of(L1_PREFIX))
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    check_fresh(name)
