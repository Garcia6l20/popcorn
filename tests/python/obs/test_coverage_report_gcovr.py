"""popcorn.coverage.report gcovr JSON: `render_gcovr_json` content, paths, spans and shape."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

import jsonschema
import pytest
from popcorn.coverage.report import (
    GCOVR_FORMAT_VERSION,
    display_name,
    parse_lcov,
    render_cobertura,
    render_gcovr_json,
)

ROOT = Path("/work/proj")

TRACEFILE = """TN:
SF:/work/proj/src/driver.c
FN:1,4,add
FN:6,sub
FNDA:3,add
FNDA:0,sub
DA:1,3
DA:2,3
DA:3,0
DA:7,0
DA:9,0
BRDA:2,0,0,1
BRDA:2,0,1,-
BRDA:2,1,0,0
end_of_record
TN:
SF:/opt/sdk/lib/a.c
DA:1,5
end_of_record
"""

SCHEMA: dict[str, Any] = {
    "type": "object",
    "required": ["gcovr/format_version", "files"],
    "additionalProperties": False,
    "properties": {
        "gcovr/format_version": {"const": "0.15"},
        "files": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["file", "lines", "functions"],
                "additionalProperties": False,
                "properties": {
                    "file": {"type": "string"},
                    "lines": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "required": ["line_number", "count", "branches"],
                            "additionalProperties": False,
                            "properties": {
                                "line_number": {"type": "integer"},
                                "count": {"type": "integer"},
                                "function_name": {"type": "string"},
                                "branches": {
                                    "type": "array",
                                    "items": {
                                        "type": "object",
                                        "required": ["count", "fallthrough", "throw"],
                                        "additionalProperties": False,
                                        "properties": {
                                            "count": {"type": "integer"},
                                            "fallthrough": {"type": "boolean"},
                                            "throw": {"type": "boolean"},
                                            "source_block_id": {"type": "integer"},
                                            "branchno": {"type": "integer"},
                                        },
                                    },
                                },
                            },
                        },
                    },
                    "functions": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "required": ["lineno"],
                            "additionalProperties": False,
                            "properties": {
                                "name": {"type": "string"},
                                "demangled_name": {"type": "string"},
                                "lineno": {"type": "integer"},
                                "execution_count": {"type": "integer"},
                            },
                        },
                    },
                },
            },
        },
    },
}


def render(text: str = TRACEFILE, root: Path = ROOT) -> str:
    return render_gcovr_json(parse_lcov(text), root=root)


def document(text: str = TRACEFILE, root: Path = ROOT) -> dict[str, Any]:
    result: dict[str, Any] = json.loads(render(text, root))
    return result


def lines(entry: dict[str, Any]) -> dict[int, dict[str, Any]]:
    return {line["line_number"]: line for line in entry["lines"]}


def test_exact_document() -> None:
    expected: dict[str, Any] = {
        "gcovr/format_version": GCOVR_FORMAT_VERSION,
        "files": [
            {
                "file": "src/driver.c",
                "functions": [
                    {"demangled_name": "add", "execution_count": 3, "lineno": 1, "name": "add"},
                    {"demangled_name": "sub", "execution_count": 0, "lineno": 6, "name": "sub"},
                ],
                "lines": [
                    {"branches": [], "count": 3, "function_name": "add", "line_number": 1},
                    {
                        "branches": [
                            {
                                "branchno": 0,
                                "count": 1,
                                "fallthrough": False,
                                "source_block_id": 0,
                                "throw": False,
                            },
                            {
                                "branchno": 1,
                                "count": 0,
                                "fallthrough": False,
                                "source_block_id": 0,
                                "throw": False,
                            },
                            {
                                "branchno": 2,
                                "count": 0,
                                "fallthrough": False,
                                "source_block_id": 1,
                                "throw": False,
                            },
                        ],
                        "count": 3,
                        "function_name": "add",
                        "line_number": 2,
                    },
                    {"branches": [], "count": 0, "function_name": "add", "line_number": 3},
                    {"branches": [], "count": 0, "function_name": "sub", "line_number": 7},
                    {"branches": [], "count": 0, "function_name": "sub", "line_number": 9},
                ],
            },
            {
                "file": "/opt/sdk/lib/a.c",
                "functions": [],
                "lines": [{"branches": [], "count": 5, "line_number": 1}],
            },
        ],
    }
    assert document() == expected


def test_branch_counts_and_numbering() -> None:
    branches = lines(document()["files"][0])[2]["branches"]
    assert [b["count"] for b in branches] == [1, 0, 0]
    assert [b["branchno"] for b in branches] == [0, 1, 2]


def test_dash_branch_counts_zero_not_previous() -> None:
    text = "SF:a.c\nDA:1,9\nBRDA:1,0,0,5\nBRDA:1,0,1,-\nend_of_record\n"
    branches = lines(document(text)["files"][0])[1]["branches"]
    assert [b["count"] for b in branches] == [5, 0]


def test_branchno_restarts_per_line() -> None:
    text = "SF:a.c\nDA:1,1\nDA:2,1\nBRDA:1,0,0,1\nBRDA:2,0,0,1\nBRDA:2,0,1,1\nend_of_record\n"
    entry = document(text)["files"][0]
    assert [b["branchno"] for b in lines(entry)[1]["branches"]] == [0]
    assert [b["branchno"] for b in lines(entry)[2]["branches"]] == [0, 1]


def test_open_function_spans_to_next_start_and_last_to_last_line() -> None:
    text = "SF:a.c\nFN:1,one\nFN:4,two\nDA:1,1\nDA:3,1\nDA:4,1\nDA:12,1\nend_of_record\n"
    names = {n: line.get("function_name") for n, line in lines(document(text)["files"][0]).items()}
    assert names == {1: "one", 3: "one", 4: "two", 12: "two"}


def names_of(text: str) -> dict[int, str | None]:
    return {n: line.get("function_name") for n, line in lines(document(text)["files"][0]).items()}


def test_nested_span_names_its_lines_and_the_outer_resumes() -> None:
    text = "SF:a.c\nFN:5,8,inner\nFN:1,20,outer\nDA:1,1\nDA:6,1\nDA:10,1\nend_of_record\n"
    assert names_of(text) == {1: "outer", 6: "inner", 10: "outer"}


def test_open_spans_given_out_of_order_run_to_the_next_start() -> None:
    text = "SF:a.c\nFN:6,b\nFN:1,a\nDA:1,1\nDA:5,1\nDA:6,1\nDA:9,1\nend_of_record\n"
    assert names_of(text) == {1: "a", 5: "a", 6: "b", 9: "b"}


def test_open_span_skips_a_function_on_its_own_start_line() -> None:
    text = "SF:a.c\nFN:10,a\nFN:10,12,b\nDA:10,1\nDA:13,1\nend_of_record\n"
    assert names_of(text) == {10: "b", 13: "a"}


def test_naming_lines_costs_no_more_than_the_lines_themselves() -> None:
    functions = "".join(f"FN:{i * 20 + 1},f{i}\n" for i in range(3000))
    das = "".join(f"DA:{n},1\n" for n in range(1, 60001))
    plain = f"SF:a.c\n{das}end_of_record\n"
    named = f"SF:a.c\n{functions}{das}end_of_record\n"
    begin = time.perf_counter()
    document(plain)
    baseline = time.perf_counter() - begin
    begin = time.perf_counter()
    entry = document(named)["files"][0]
    assert time.perf_counter() - begin < 5 * baseline
    assert lines(entry)[60000]["function_name"] == "f2999"


def test_closed_function_leaves_outside_lines_unnamed() -> None:
    text = "SF:a.c\nFN:2,3,f\nDA:1,1\nDA:2,1\nDA:3,1\nDA:4,1\nend_of_record\n"
    names = {n: line.get("function_name") for n, line in lines(document(text)["files"][0]).items()}
    assert names == {1: None, 2: "f", 3: "f", 4: None}


def test_paths_relative_under_root_absolute_outside() -> None:
    files = [entry["file"] for entry in document()["files"]]
    assert files == ["src/driver.c", "/opt/sdk/lib/a.c"]


def test_path_agrees_with_display_name_and_xml() -> None:
    report = parse_lcov(TRACEFILE)
    names = [entry["file"] for entry in json.loads(render_gcovr_json(report, root=ROOT))["files"]]
    assert names == [display_name(file.path, ROOT) for file in report.files]
    xml = render_cobertura(report, root=ROOT, timestamp=0)
    assert all(f'filename="{name}"' in xml for name in names)


def test_miss_mode_zero_counts_and_hit_unchanged() -> None:
    text = "SF:a.c\nFN:1,3,f\nFNDA:4,f\nDA:1,4\nDA:2,4\nBRDA:2,0,0,4\nend_of_record\n"
    report = parse_lcov(text).attached({("a.c", 2): ()})
    hit = json.loads(render_gcovr_json(report.with_unmodelled("hit"), root=ROOT))
    miss = json.loads(render_gcovr_json(report.with_unmodelled("miss"), root=ROOT))
    assert lines(hit["files"][0])[2]["count"] == 4
    assert lines(hit["files"][0])[2]["branches"][0]["count"] == 4
    assert lines(miss["files"][0])[2]["count"] == 0
    assert lines(miss["files"][0])[2]["branches"][0]["count"] == 0
    assert lines(miss["files"][0])[1]["count"] == 4


def test_empty_report() -> None:
    assert document("") == {"gcovr/format_version": GCOVR_FORMAT_VERSION, "files": []}


def test_brda_without_da_names_file_and_line() -> None:
    text = "SF:a.c\nDA:1,1\nBRDA:7,0,0,1\nend_of_record\n"
    with pytest.raises(ValueError, match=r"a\.c:7"):
        render(text)


def test_output_is_stable_sorted_and_newline_terminated() -> None:
    first = render()
    assert render() == first
    assert first.endswith("}\n")
    assert first == json.dumps(json.loads(first), indent=1, sort_keys=True) + "\n"


def test_matches_schema() -> None:
    jsonschema.validate(document(), SCHEMA)
    broken = document()
    broken["gcovr/format_version"] = "0.14"
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(broken, SCHEMA)
