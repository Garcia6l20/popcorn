import dataclasses
from typing import Any, cast

import jsonschema
import pytest
import reports
from popcorn.cli._cpuprofile import to_cpuprofile
from popcorn.profiling import FrameKind, FrameRow

SCHEMA = {
    "type": "object",
    "required": ["nodes", "startTime", "endTime", "samples", "timeDeltas"],
    "properties": {
        "nodes": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id", "callFrame", "hitCount", "children"],
                "properties": {
                    "id": {"type": "integer", "minimum": 1},
                    "hitCount": {"type": "integer", "minimum": 0},
                    "children": {"type": "array", "items": {"type": "integer"}},
                    "callFrame": {
                        "type": "object",
                        "required": [
                            "functionName",
                            "scriptId",
                            "url",
                            "lineNumber",
                            "columnNumber",
                        ],
                    },
                },
            },
        },
        "startTime": {"type": "integer"},
        "endTime": {"type": "integer"},
        "samples": {"type": "array", "items": {"type": "integer"}},
        "timeDeltas": {"type": "array", "items": {"type": "integer"}},
    },
}


def build() -> dict[str, Any]:
    return cast(dict[str, Any], to_cpuprofile(reports.tree_report()))


def by_id(profile: dict[str, Any]) -> dict[int, dict[str, Any]]:
    return {node["id"]: node for node in profile["nodes"]}


def subtree(profile: dict[str, Any], node_id: int) -> set[int]:
    nodes = by_id(profile)
    found: set[int] = set()
    stack: list[int] = [node_id]
    while stack:
        current = stack.pop()
        found.add(current)
        stack.extend(nodes[current]["children"])
    return found


def test_matches_the_schema() -> None:
    jsonschema.validate(build(), SCHEMA)


def test_structure() -> None:
    profile = build()
    nodes = profile["nodes"]
    count = len(reports.TREE)
    assert [node["id"] for node in nodes] == list(range(1, count + 1))
    assert nodes[0]["callFrame"]["functionName"] == "(root)"
    listed = [child for node in nodes for child in node["children"]]
    assert sorted(listed) == list(range(2, count + 1))
    assert subtree(profile, 1) == set(range(1, count + 1))


def test_totals_and_samples() -> None:
    profile = build()
    total = sum(reports.TREE_SELF)
    assert sum(node["hitCount"] for node in profile["nodes"]) == total
    assert profile["startTime"] == 1
    assert profile["endTime"] == profile["startTime"] + total
    assert len(profile["samples"]) == len(profile["timeDeltas"])
    expected = [index + 1 for index, value in enumerate(reports.TREE_SELF) if value > 0]
    assert sorted(profile["samples"][:-1]) == expected


def test_each_subtree_is_one_contiguous_range_of_its_inclusive_weight() -> None:
    profile = build()
    frames = reports.tree_frames()
    nodes = by_id(profile)
    samples = profile["samples"]
    for node_id in nodes:
        members = subtree(profile, node_id)
        positions = [i for i, sample in enumerate(samples) if sample in members]
        if not positions:
            continue
        assert positions == list(range(positions[0], positions[-1] + 1))
        assert (
            sum(nodes[samples[i]]["hitCount"] for i in positions) == frames[node_id - 1].inclusive
        )


def test_independent_v8_reader_recovers_self() -> None:
    profile = build()
    samples, deltas = profile["samples"], profile["timeDeltas"]
    starts: list[int] = []
    clock = profile["startTime"]
    for delta in deltas:
        clock += delta
        starts.append(clock)
    ends: list[int] = [*starts[1:], profile["endTime"]]
    for sample, start, end in zip(samples, starts, ends, strict=True):
        assert end - start == reports.TREE_SELF[sample - 1]


def test_names_urls_and_placements() -> None:
    nodes = by_id(build())
    names = {node["callFrame"]["functionName"] for node in nodes.values()}
    assert "[SysTick]" in names
    assert "a;2" in names
    assert nodes[8]["callFrame"]["url"] == "exception"
    assert nodes[9]["callFrame"]["url"] == "entry"
    assert nodes[9]["callFrame"]["functionName"] == "handler"
    assert nodes[4]["callFrame"]["url"] == "call"
    assert nodes[1]["callFrame"]["url"] == ""
    assert 9 in nodes[8]["children"]
    assert 10 in nodes[6]["children"]
    assert 2 in nodes[1]["children"]


def test_no_samples_gives_empty_arrays() -> None:
    frames = reports.tree_frames((0,) * len(reports.TREE_SELF))
    profile = to_cpuprofile(reports.tree_report(frames))
    assert profile["samples"] == []
    assert profile["timeDeltas"] == []
    assert profile["endTime"] == profile["startTime"]


def test_without_hotspots_raises() -> None:
    with pytest.raises(ValueError, match="hotspots"):
        to_cpuprofile(reports.synthetic(hotspots=False))


def bad(frames: tuple[FrameRow, ...]) -> None:
    with pytest.raises(ValueError):
        to_cpuprofile(reports.tree_report(frames))


def test_malformed_frames_raise() -> None:
    good = reports.tree_frames()
    bad(())
    bad((dataclasses.replace(good[0], kind=FrameKind.ENTRY), *good[1:]))
    for parent in (-1, 2, 5):
        bad((*good[:2], dataclasses.replace(good[2], parent=parent), *good[3:]))


def test_the_fixture_header_agrees_with_its_tree() -> None:
    report = reports.tree_report()
    assert report.hotspots is not None
    total = sum(reports.TREE_SELF)
    assert report.executed == report.elapsed_ticks == total == 45
    assert report.clocks[0].instructions == total
    assert sum(row.self for row in report.hotspots.functions) == total


def test_a_root_sample_closes_the_last_real_sample_at_the_end_time() -> None:
    profile = build()
    samples, deltas = profile["samples"], profile["timeDeltas"]
    assert samples[-1] == 1
    assert by_id(profile)[1]["hitCount"] == 0
    assert deltas[-1] == reports.TREE_SELF[samples[-2] - 1]
    assert profile["startTime"] + sum(deltas) == profile["endTime"]
