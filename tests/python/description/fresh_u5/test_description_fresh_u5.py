"""The U5 shipped descriptions against a fresh ship build."""

import pytest
from popcorn.stm32.description._shipped import shipped_names

from ..fresh_common import H5_PREFIX, L1_PREFIX, U5_PREFIX, check_fresh, names_of


@pytest.mark.parametrize("name", names_of(U5_PREFIX))
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    check_fresh(name)


def test_the_u5_h5_and_l1_lists_together_hold_every_shipped_part() -> None:
    u5, h5, l1 = names_of(U5_PREFIX), names_of(H5_PREFIX), names_of(L1_PREFIX)
    assert len({*u5, *h5, *l1}) == len(u5) + len(h5) + len(l1)
    assert sorted(u5 + h5 + l1) == sorted(shipped_names())
    assert u5
    assert h5
    assert l1
