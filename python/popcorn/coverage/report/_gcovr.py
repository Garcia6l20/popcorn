"""The gcovr JSON tracefile of a coverage report."""

from __future__ import annotations

import json
from bisect import bisect_left, bisect_right
from collections import defaultdict
from pathlib import Path
from typing import Any

from popcorn.coverage.report._model import (
    BranchRecord,
    FunctionRecord,
    LcovFile,
    LcovReport,
    display_name,
)

GCOVR_FORMAT_VERSION = "0.15"


def _spans(file: LcovFile) -> list[tuple[int, int, str]]:
    ordered = sorted(file.functions, key=lambda function: function.start)
    starts = [function.start for function in ordered]
    last = max(file.lines, default=0)
    spans: list[tuple[int, int, str]] = []
    for function in ordered:
        if function.end is not None:
            end = function.end
        else:
            later = bisect_right(starts, function.start)
            end = starts[later] - 1 if later < len(starts) else last
        spans.append((function.start, end, function.name))
    return spans


def _line_names(file: LcovFile) -> dict[int, str]:
    numbers = sorted(file.lines)
    names: dict[int, str] = {}
    for start, end, name in _spans(file):
        for number in numbers[bisect_left(numbers, start) : bisect_right(numbers, end)]:
            names[number] = name
    return names


def _function(function: FunctionRecord) -> dict[str, Any]:
    return {
        "name": function.name,
        "demangled_name": function.name,
        "lineno": function.start,
        "execution_count": function.hits,
    }


def _branches(file: LcovFile, path: str) -> dict[int, list[dict[str, Any]]]:
    by_line: dict[int, list[BranchRecord]] = defaultdict(list)
    for branch in file.branches:
        if branch.line not in file.lines:
            raise ValueError(f"{path}:{branch.line}: a BRDA record on a line with no DA record")
        by_line[branch.line].append(branch)
    return {
        number: [
            {
                "count": branch.taken or 0,
                "fallthrough": False,
                "throw": False,
                "source_block_id": branch.block,
                "branchno": index,
            }
            for index, branch in enumerate(branches)
        ]
        for number, branches in by_line.items()
    }


def _file(file: LcovFile, path: str) -> dict[str, Any]:
    names = _line_names(file)
    branches = _branches(file, path)
    lines: list[dict[str, Any]] = []
    for number in sorted(file.lines):
        line: dict[str, Any] = {
            "line_number": number,
            "count": file.lines[number],
            "branches": branches.get(number, []),
        }
        if number in names:
            line["function_name"] = names[number]
        lines.append(line)
    return {
        "file": path,
        "lines": lines,
        "functions": [_function(function) for function in file.functions],
    }


def render_gcovr_json(report: LcovReport, *, root: Path) -> str:
    """The gcovr JSON tracefile of `report`, as text with a trailing newline.

    The document targets gcovr 8.8, which reads a tracefile whose `gcovr/format_version` is
    exactly `GCOVR_FORMAT_VERSION`. Keys are sorted and the text is the same for the same report.

    A file is named by `display_name`: relative to `root` when it lies under it, else as written.
    Run gcovr with `--root` set to `root` to find the sources. A `DA` record is a line with its
    hit count. A `BRDA` record is a branch of its line: `count` is the taken count, 0 for a
    branch that never ran, `source_block_id` is the block and `branchno` the index on the line.
    An `FN` record is a function with its start line and `FNDA` hits. A line inside the span of a
    function carries its `function_name`, where the span runs from the start line to the `FN`
    end line, to the line before the next later start when the record has none, and to the last
    line of the file for the last function. When spans overlap the function that starts last
    names the line.

    Raises:
        ValueError: A file holds a `BRDA` record on a line with no `DA` record. The message names
            the file and the line.
    """
    files = [_file(file, display_name(file.path, root)) for file in report.files]
    document = {"gcovr/format_version": GCOVR_FORMAT_VERSION, "files": files}
    return json.dumps(document, indent=1, sort_keys=True) + "\n"
