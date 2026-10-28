"""The unmodelled modes in the report model, term, built-in html and the dispatch."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from popcorn.coverage import unmodelled_path
from popcorn.coverage.report import (
    ReportSpec,
    UnmodelledUse,
    dump_unmodelled,
    generate,
    parse_lcov,
    parse_unmodelled,
    render_html,
    render_term,
)

from .test_coverage_report_spec import fake_genhtml

TRACEFILE = """TN:
SF:src/a.c
FN:1,4,f
FN:6,9,g
FN:11,12,h
FNDA:3,f
FNDA:2,g
FNDA:1,h
FNF:3
FNH:3
BRDA:2,0,0,3
BRDA:2,0,1,-
BRDA:3,0,0,1
DA:1,3
DA:2,3
DA:3,1
DA:7,2
DA:8,2
DA:11,1
LF:6
LH:6
end_of_record
TN:
SF:src/b.c
DA:1,1
end_of_record
"""

SOURCE = "int f(void) {\n  if (x) return 1;\n  return *reg;\n}\n"


def use(register: str = "DATA", hits: int = 4) -> UnmodelledUse:
    return UnmodelledUse("UART", register, 0, False, "no_model", hits)


def sidecar(unplaced: int = 0) -> str:
    uses = {("src/a.c", 2): (use(),), ("src/a.c", 7): (use("STAT"),), ("src/a.c", 8): (use("X"),)}
    return dump_unmodelled(uses, unplaced)


def attached(mode: str, unplaced: int = 0):
    uses, count = parse_unmodelled(sidecar(unplaced))
    return parse_lcov(TRACEFILE).attached(uses, count).with_unmodelled(mode)


def test_the_sidecar_round_trips_and_defaults_unplaced_to_zero() -> None:
    uses, unplaced = parse_unmodelled(sidecar(5))
    assert unplaced == 5
    assert dump_unmodelled(uses, unplaced) == sidecar(5)
    old = json.dumps({"version": 1, "files": {}})
    assert parse_unmodelled(old) == ({}, 0)


@pytest.mark.parametrize("text", ["{}", '{"version": 2, "files": {}}', '{"version": 1}', "[]"])
def test_a_bad_sidecar_is_a_value_error(text: str) -> None:
    with pytest.raises(ValueError):
        parse_unmodelled(text)


def test_only_lines_with_a_da_record_attach() -> None:
    uses, _ = parse_unmodelled(
        dump_unmodelled({("src/a.c", 2): (use(),), ("src/a.c", 5): (use(),), ("zz.c", 1): (use(),)})
    )
    report = parse_lcov(TRACEFILE).attached(uses)
    assert set(report.unmodelled) == {("src/a.c", 2)}


def test_hit_drops_the_data_and_mark_keeps_it() -> None:
    plain = parse_lcov(TRACEFILE)
    assert attached("hit", 3) == plain
    marked = attached("mark", 3)
    assert marked.files == plain.files
    assert len(marked.unmodelled) == 3
    assert marked.unplaced == 3


def test_miss_zeroes_lines_branches_and_functions_that_have_no_other_hit() -> None:
    missed = attached("miss")
    a, _ = missed.files
    assert a.lines == {1: 3, 2: 0, 3: 1, 7: 0, 8: 0, 11: 1}
    functions = {function.name: function.hits for function in a.functions}
    assert functions == {"f": 3, "g": 0, "h": 1}
    branches = {(b.line, b.branch): b.taken for b in a.branches}
    assert branches == {(2, 0): 0, (2, 1): None, (3, 0): 1}
    assert missed.unmodelled == {}
    assert missed.totals().lines.hit == 4


def test_miss_keeps_a_function_with_another_hit_line_and_zeroes_a_function_with_none() -> None:
    uses = {("src/a.c", 1): (use(),), ("src/a.c", 2): (use(),), ("src/a.c", 3): (use(),)}
    report = parse_lcov(TRACEFILE).attached(uses).with_unmodelled("miss")
    assert {f.name: f.hits for f in report.files[0].functions}["f"] == 0


def test_a_function_without_an_end_spans_to_the_next_start() -> None:
    text = "SF:a.c\nFN:1,f\nFN:5,g\nFNDA:2,f\nFNDA:1,g\nDA:2,2\nDA:3,2\nDA:6,1\nend_of_record\n"
    uses = {("a.c", 2): (use(),), ("a.c", 3): (use(),)}
    report = parse_lcov(text).attached(uses).with_unmodelled("miss")
    assert {f.name: f.hits for f in report.files[0].functions} == {"f": 0, "g": 1}


def test_an_unknown_mode_is_refused() -> None:
    with pytest.raises(ValueError, match="mark, miss, hit"):
        parse_lcov(TRACEFILE).with_unmodelled("maybe")


def test_filtering_drops_the_lines_of_omitted_files() -> None:
    report = attached("mark").filtered([], ["src/a.c"], Path.cwd())
    assert report.unmodelled == {}


def test_term_adds_a_column_in_mark_and_a_line_for_unplaced_accesses() -> None:
    text = render_term(attached("mark", 7), root=Path("/r"))
    lines = text.splitlines()
    assert lines[0].split()[-1] == "Unmodelled"
    assert lines[2].split()[-1] == "3"
    assert lines[3].split()[-1] == "0"
    assert lines[5].split()[-1] == "3"
    assert lines[6] == "7 unmodelled accesses came from code with no line in this report"
    plain = render_term(attached("hit"), root=Path("/r"))
    assert "Unmodelled" not in plain
    assert "no line in this report" not in plain
    assert "Unmodelled" not in render_term(attached("miss"), root=Path("/r"))


def test_html_marks_the_lines_with_an_escaped_tooltip(tmp_path: Path) -> None:
    root = tmp_path / "root"
    (root / "src").mkdir(parents=True)
    (root / "src" / "a.c").write_text(SOURCE)
    uses = {("src/a.c", 3): (UnmodelledUse('U"<A>', "R&", 0, False, "no_model", 4), use("B", 2))}
    report = parse_lcov(TRACEFILE).attached(uses).with_unmodelled("mark")
    out = tmp_path / "out"
    render_html(report, out, root=root)
    page = (out / "src-0.html").read_text()
    row = next(line for line in page.splitlines() if 'class="no">3<' in line)
    assert row.startswith('<tr class="unmodelled" title="')
    assert 'U&quot;&lt;A&gt;.R&amp; (no_model, 4 hits)&#10;UART.B (no_model, 2 hits)"' in row
    assert "1 lines accessed unmodelled hardware" in page
    assert 'class="hit"' in page
    assert "unmodelled" not in (out / "src-1.html").read_text()


def test_html_without_marks_has_no_unmodelled_row(tmp_path: Path) -> None:
    render_html(attached("hit"), tmp_path / "out", root=tmp_path)
    assert "unmodelled" not in (tmp_path / "out" / "src-0.html").read_text().replace("tr.unm", "")


def lcov_with_sidecar(tmp_path: Path, unplaced: int = 0) -> Path:
    path = tmp_path / "in.info"
    path.write_text(TRACEFILE)
    unmodelled_path(path).write_text(sidecar(unplaced))
    return path


def test_mark_writes_html_with_the_builtin_renderer_even_with_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    log = fake_genhtml(tmp_path, monkeypatch)
    out = tmp_path / "out"
    generate(
        lcov_with_sidecar(tmp_path),
        [ReportSpec("html", out)],
        root=tmp_path,
        unmodelled="mark",
    )
    assert (out / "src-0.html").is_file()
    assert not log.exists()


@pytest.mark.parametrize("mode", ["hit", "miss"])
def test_hit_and_miss_html_still_run_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, mode: str
) -> None:
    log = fake_genhtml(tmp_path, monkeypatch)
    out = tmp_path / "out"
    generate(lcov_with_sidecar(tmp_path), [ReportSpec("html", out)], root=tmp_path, unmodelled=mode)
    assert log.exists()
    assert not (out / "src-0.html").exists()
    if mode == "miss":
        assert "DA:2,0" in json.loads(log.read_text())["lcov"]


def test_mark_without_a_marked_line_runs_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    log = fake_genhtml(tmp_path, monkeypatch)
    lcov = tmp_path / "in.info"
    lcov.write_text(TRACEFILE)
    unmodelled_path(lcov).write_text(dump_unmodelled({}, 2))
    generate(lcov, [ReportSpec("html", tmp_path / "out")], root=tmp_path, unmodelled="mark")
    assert log.exists()


def test_the_lcov_report_gets_the_sidecar_of_the_kept_files(tmp_path: Path) -> None:
    lcov = lcov_with_sidecar(tmp_path, 3)
    target = tmp_path / "o" / "x.info"
    generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="mark")
    assert target.read_bytes() == lcov.read_bytes()
    assert unmodelled_path(target).read_text() == sidecar(3)
    kept = tmp_path / "o" / "y.info"
    generate(lcov, [ReportSpec("lcov", kept)], root=tmp_path, unmodelled="mark", omit=["src/a.c"])
    assert parse_unmodelled(unmodelled_path(kept).read_text()) == ({}, 3)
    generate(lcov, [ReportSpec("lcov", tmp_path / "o" / "z.info")], root=tmp_path)
    assert not unmodelled_path(tmp_path / "o" / "z.info").exists()


def test_a_report_without_a_sidecar_removes_the_one_of_an_earlier_run(tmp_path: Path) -> None:
    lcov = lcov_with_sidecar(tmp_path, 3)
    target = tmp_path / "o" / "x.info"
    generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="mark")
    assert unmodelled_path(target).is_file()
    generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="hit")
    assert not unmodelled_path(target).exists()
    generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="mark")
    unmodelled_path(lcov).unlink()
    generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="mark")
    assert not unmodelled_path(target).exists()


def test_miss_rewrites_the_lcov_report(tmp_path: Path) -> None:
    lcov = lcov_with_sidecar(tmp_path)
    target = tmp_path / "m.info"
    result = generate(lcov, [ReportSpec("lcov", target)], root=tmp_path, unmodelled="miss")
    assert "DA:2,0\n" in target.read_text()
    assert "FNDA:0,g\n" in target.read_text()
    assert "BRDA:2,0,0,0\n" in target.read_text()
    assert result.totals.lines.hit == 4


def test_a_corrupt_sidecar_names_its_file(tmp_path: Path) -> None:
    lcov = tmp_path / "in.info"
    lcov.write_text(TRACEFILE)
    unmodelled_path(lcov).write_text("{")
    with pytest.raises(ValueError, match=r"in\.info\.unmodelled\.json"):
        generate(lcov, [ReportSpec("term")], root=tmp_path, unmodelled="mark")


def gcovr_lines(path: Path, name: str) -> dict[int, int]:
    document = json.loads(path.read_text())
    (entry,) = [entry for entry in document["files"] if entry["file"] == name]
    return {line["line_number"]: line["count"] for line in entry["lines"]}


def test_the_gcovr_report_keeps_counts_in_mark_and_writes_the_sidecar(tmp_path: Path) -> None:
    lcov = lcov_with_sidecar(tmp_path, 3)
    target = tmp_path / "o" / "g.json"
    generate(lcov, [ReportSpec("gcovr", target)], root=tmp_path, unmodelled="mark")
    assert gcovr_lines(target, "src/a.c")[2] == 3
    assert unmodelled_path(target).read_text() == sidecar(3)


def test_the_gcovr_report_zeroes_the_marked_lines_in_miss(tmp_path: Path) -> None:
    lcov = lcov_with_sidecar(tmp_path)
    target = tmp_path / "g.json"
    generate(lcov, [ReportSpec("gcovr", target)], root=tmp_path, unmodelled="miss")
    counts = gcovr_lines(target, "src/a.c")
    assert (counts[2], counts[7], counts[8], counts[1]) == (0, 0, 0, 3)
    assert unmodelled_path(target).read_text() == sidecar()


def test_the_gcovr_report_in_hit_keeps_the_counts_and_removes_an_old_sidecar(
    tmp_path: Path,
) -> None:
    lcov = lcov_with_sidecar(tmp_path)
    target = tmp_path / "g.json"
    generate(lcov, [ReportSpec("gcovr", target)], root=tmp_path, unmodelled="mark")
    assert unmodelled_path(target).is_file()
    generate(lcov, [ReportSpec("gcovr", target)], root=tmp_path, unmodelled="hit")
    assert gcovr_lines(target, "src/a.c")[2] == 3
    assert not unmodelled_path(target).exists()


def test_the_gcovr_report_filters_and_refuses_a_bad_tracefile_before_writing(
    tmp_path: Path,
) -> None:
    lcov = lcov_with_sidecar(tmp_path)
    target = tmp_path / "g.json"
    other = tmp_path / "o.info"
    generate(lcov, [ReportSpec("gcovr", target)], root=tmp_path, omit=["src/b.c"])
    assert [entry["file"] for entry in json.loads(target.read_text())["files"]] == ["src/a.c"]
    lcov.write_text("SF:a.c\nDA:1,1\nBRDA:9,0,0,1\nend_of_record\n")
    with pytest.raises(ValueError, match=r"a\.c:9"):
        generate(lcov, [ReportSpec("lcov", other), ReportSpec("gcovr", target)], root=tmp_path)
    assert not other.exists()
