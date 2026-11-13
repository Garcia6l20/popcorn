"""Fixtures shared by the DWARF reader tests."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest
from elftools.dwarf.locationlists import LocationLists
from elftools.dwarf.ranges import RangeLists


def _counted(original: Callable[..., Any], kind: str, reads: dict[str, int]) -> Callable[..., Any]:
    def counted(*args: Any, **kwargs: Any) -> Any:
        reads[kind] += 1
        return original(*args, **kwargs)

    return counted


@pytest.fixture
def list_reads(monkeypatch: pytest.MonkeyPatch) -> dict[str, int]:
    """The location lists and range lists pyelftools reads from here on, counted by kind."""
    reads = {"location": 0, "range": 0}
    for kind, owner, name in (
        ("location", LocationLists, "get_location_list_at_offset"),
        ("range", RangeLists, "get_range_list_at_offset"),
    ):
        monkeypatch.setattr(owner, name, _counted(getattr(owner, name), kind, reads))
    return reads
