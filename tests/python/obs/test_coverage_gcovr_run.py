"""The gcovr JSON tracefile read by a real gcovr: pages, totals, merging and the root rule."""

from __future__ import annotations

import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest
from popcorn.coverage.report import GCOVR_FORMAT_VERSION, parse_lcov, render_gcovr_json

SOURCE_A = "int f(void) {\n  if (x) return 1;\n  return 2;\n}\nint g(void) {\n  return 3;\n}\n"
SOURCE_B = "int h(void) {\n  return 4;\n}\n"

FIRST = """TN:
SF:src/a.c
FN:1,4,f
FN:5,7,g
FNDA:2,f
FNDA:0,g
DA:1,2
DA:2,2
DA:3,0
DA:5,0
DA:6,0
BRDA:2,0,0,2
BRDA:2,0,1,-
end_of_record
TN:
SF:src/b.c
FN:1,3,h
FNDA:1,h
DA:1,1
DA:2,1
end_of_record
"""

SECOND = (
    FIRST.replace("FNDA:2,f", "FNDA:5,f")
    .replace("DA:1,2", "DA:1,5")
    .replace("DA:2,2", "DA:2,5")
    .replace("BRDA:2,0,0,2", "BRDA:2,0,0,5")
)

SHIFTED = SECOND.replace("FN:1,4,f", "FN:2,4,f")


def gcovr(cwd: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    done = subprocess.run(
        [sys.executable, "-m", "gcovr", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if check:
        assert done.returncode == 0, done.stderr
    return done


def project(tmp_path: Path, name: str = "root") -> Path:
    root = tmp_path / name
    (root / "src").mkdir(parents=True)
    (root / "src" / "a.c").write_text(SOURCE_A)
    (root / "src" / "b.c").write_text(SOURCE_B)
    return root


def tracefile(root: Path, text: str, name: str) -> Path:
    path = root.parent / name
    path.write_text(render_gcovr_json(parse_lcov(text), root=root))
    return path


def text_of(page: Path) -> str:
    return html.unescape(re.sub(r"<[^>]*>", "", page.read_text()))


def summary(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = json.loads(path.read_text())
    return result


def test_html_details_pages_and_totals_match_the_report(tmp_path: Path) -> None:
    root = project(tmp_path)
    trace = tracefile(root, FIRST, "t.json")
    out = tmp_path / "out"
    out.mkdir()
    gcovr(
        tmp_path,
        "--root",
        str(root),
        "--json-add-tracefile",
        str(trace),
        "--html-details",
        str(out / "cov.html"),
        "--json-summary",
        str(out / "s.json"),
    )
    pages = sorted(path.name for path in out.glob("cov.a.c.*.html"))
    assert len(pages) == 1
    assert "if (x) return 1;" in text_of(out / pages[0])
    functions = (out / "cov.functions.html").read_text()
    for name, where in (("f", "src/a.c:1"), ("g", "src/a.c:5"), ("h", "src/b.c:1")):
        assert f">{name} ({where})<" in functions
    totals = parse_lcov(FIRST).totals()
    data = summary(out / "s.json")
    assert data["line_total"] == totals.lines.found
    assert data["line_covered"] == totals.lines.hit
    assert data["branch_total"] == totals.branches.found
    assert data["branch_covered"] == totals.branches.hit
    assert data["function_total"] == totals.functions.found
    assert data["function_covered"] == totals.functions.hit


def test_two_tracefiles_merge_by_summing_counts(tmp_path: Path) -> None:
    root = project(tmp_path)
    first = tracefile(root, FIRST, "a.json")
    second = tracefile(root, SECOND, "b.json")
    merged = tmp_path / "m.json"
    gcovr(
        tmp_path,
        "--root",
        str(root),
        "--json-add-tracefile",
        str(first),
        "--json-add-tracefile",
        str(second),
        "--json",
        str(merged),
    )
    (entry,) = [e for e in summary(merged)["files"] if e["file"] == "src/a.c"]
    counts = {line["line_number"]: line["count"] for line in entry["lines"]}
    assert counts == {1: 7, 2: 7, 3: 0, 5: 0, 6: 0}
    branches = [b["count"] for line in entry["lines"] for b in line["branches"]]
    assert branches == [7, 0]


def test_a_function_on_different_lines_needs_separate_merge_mode(tmp_path: Path) -> None:
    root = project(tmp_path)
    first = tracefile(root, FIRST, "a.json")
    second = tracefile(root, SHIFTED, "c.json")
    base = ["--root", str(root), "--json-add-tracefile", str(first)]
    base += ["--json-add-tracefile", str(second), "--json", str(tmp_path / "m.json")]
    refused = gcovr(tmp_path, *base, check=False)
    assert refused.returncode != 0
    gcovr(tmp_path, *base, "--merge-mode-functions=separate")
    (entry,) = [e for e in summary(tmp_path / "m.json")["files"] if e["file"] == "src/a.c"]
    named = {
        (line["line_number"], line.get("function_name")): line["count"] for line in entry["lines"]
    }
    assert named[(2, "f")] == 7
    assert named[(5, "g")] == 0
    assert [(f["name"], f["lineno"]) for f in entry["functions"]] == [("f", 1), ("f", 2), ("g", 5)]


def test_a_format_version_mismatch_is_refused(tmp_path: Path) -> None:
    root = project(tmp_path)
    trace = tracefile(root, FIRST, "t.json")
    document = json.loads(trace.read_text())
    assert document["gcovr/format_version"] == GCOVR_FORMAT_VERSION
    document["gcovr/format_version"] = "0.0"
    trace.write_text(json.dumps(document))
    done = gcovr(
        tmp_path,
        "--root",
        str(root),
        "--json-add-tracefile",
        str(trace),
        "--json-summary",
        str(tmp_path / "s.json"),
        check=False,
    )
    assert done.returncode != 0
    assert "format version" in done.stderr


def test_an_lcov_file_is_not_a_gcovr_tracefile(tmp_path: Path) -> None:
    root = project(tmp_path)
    info = tmp_path / "x.info"
    info.write_text(FIRST)
    done = gcovr(
        tmp_path,
        "--root",
        str(root),
        "--json-add-tracefile",
        str(info),
        "--json-summary",
        str(tmp_path / "s.json"),
        check=False,
    )
    assert done.returncode != 0


def test_paths_resolve_against_the_root_given_to_gcovr(tmp_path: Path) -> None:
    root = project(tmp_path)
    trace = tracefile(root, FIRST, "t.json")
    moved = tmp_path / "moved"
    shutil.copytree(root, moved)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    out = tmp_path / "out"
    out.mkdir()
    gcovr(
        elsewhere,
        "--root",
        str(moved),
        "--json-add-tracefile",
        str(trace),
        "--html-details",
        str(out / "cov.html"),
    )
    (page,) = out.glob("cov.a.c.*.html")
    assert "return 2;" in text_of(page)


def test_an_absolute_source_outside_the_root_keeps_its_path(tmp_path: Path) -> None:
    root = project(tmp_path)
    outside = tmp_path / "sdk"
    outside.mkdir()
    (outside / "lib.c").write_text("int k(void) { return 1; }\n")
    text = f"TN:\nSF:{outside / 'lib.c'}\nDA:1,1\nend_of_record\n"
    trace = tracefile(root, text, "t.json")
    assert json.loads(trace.read_text())["files"][0]["file"] == str(outside / "lib.c")
    gcovr(
        tmp_path,
        "--root",
        str(root),
        "--json-add-tracefile",
        str(trace),
        "--json-summary",
        str(tmp_path / "s.json"),
        "--filter",
        str(outside),
    )
    assert summary(tmp_path / "s.json")["line_total"] == 1


@pytest.mark.parametrize("flag", ["--html-medium-threshold", "--html-high-threshold"])
def test_the_html_threshold_options_exist(tmp_path: Path, flag: str) -> None:
    done = gcovr(tmp_path, "--help")
    assert flag in done.stdout
