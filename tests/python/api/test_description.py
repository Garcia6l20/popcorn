"""The MCU description through the binding: region controllers."""

from __future__ import annotations

import json
from pathlib import Path

import popcorn


def test_a_description_without_controllers_loads_unchanged(m33_json: Path) -> None:
    description = popcorn.load_description(m33_json)
    assert all(region.controller is None for region in description.regions)


def test_a_region_controller_reads_back(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["regions"][0]["controller"] = "TESTDEV"
    description = popcorn.parse_description(json.dumps(raw), "m33.json")
    assert description.regions[0].controller == "TESTDEV"
    assert description.regions[1].controller is None
