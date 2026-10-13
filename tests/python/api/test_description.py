"""The part description through the binding: region controllers and content."""

from __future__ import annotations

import json
from pathlib import Path

import popcorn
import pytest


def test_a_description_without_controllers_loads_unchanged(m33_json: Path) -> None:
    description = popcorn.load_description(m33_json)
    assert all(region.controller is None for region in description.regions)


def test_a_region_controller_reads_back(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["regions"][0]["controller"] = "TESTDEV"
    description = popcorn.parse_description(json.dumps(raw), "m33.json")
    assert description.regions[0].controller == "TESTDEV"
    assert description.regions[1].controller is None


def test_a_description_without_content_loads_unchanged(m33_json: Path) -> None:
    description = popcorn.load_description(m33_json)
    assert all(region.content is None for region in description.regions)


def test_region_content_reads_back_and_fills_memory(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["regions"][0]["content"] = [{"offset": 0x7A0, "data": [0x00, 0x04]}]
    description = popcorn.parse_description(json.dumps(raw), "m33.json")
    content = description.regions[0].content
    assert content is not None
    assert [(piece.offset, list(piece.data)) for piece in content] == [(0x7A0, [0x00, 0x04])]
    machine = popcorn.Machine(description)
    start = description.regions[0].start
    assert machine.mem.read(start + 0x7A0, 4) == bytes([0x00, 0x04, 0xFF, 0xFF])


def test_region_content_past_its_region_is_a_config_error(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    size = raw["regions"][0]["size"]
    raw["regions"][0]["content"] = [{"offset": size - 1, "data": [0x00, 0x04]}]
    description = popcorn.parse_description(json.dumps(raw), "m33.json")
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(description)
    assert caught.value.key == "regions.FLASH.content[0]"
