import dataclasses
import errno
import json
import sys
from pathlib import Path

import pytest
import reports
from click.testing import CliRunner
from popcorn.cli import cli, main
from popcorn.cli._profile import SORT_FIELD, TABLES, render_diff
from popcorn.profiling import Report, ReportDiff, diff


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["profile", "diff", *map(str, args)])


def test_tables_name_every_table_the_library_emits() -> None:
    full = reports.full_report()
    assert all(getattr(full, field.name) is not None for field in dataclasses.fields(Report))
    bare = reports.synthetic(hotspots=False, main_self=1)
    assert set(diff(full, bare)) == set(TABLES)


def test_report_against_itself_prints_no_differences(tmp_path: Path) -> None:
    path = reports.write(tmp_path / "a.json", reports.full_report())
    result = run(path, path)
    assert result.exit_code == 0
    assert result.stdout == "no differences\n"


def test_a_raised_function_prints_its_signed_delta(tmp_path: Path) -> None:
    a = reports.write(tmp_path / "a.json", reports.synthetic(main_self=10))
    b = reports.write(tmp_path / "b.json", reports.synthetic(main_self=110))
    result = run("--table", "functions", a, b)
    assert result.exit_code == 0
    assert result.stdout.splitlines() == ["functions", "  main  self=+100 inclusive=+100"]
    swapped = run("--table", "functions", b, a)
    assert "self=-100" in swapped.stdout


def test_top_truncates_and_counts_the_rest(tmp_path: Path) -> None:
    a = reports.write(tmp_path / "a.json", reports.synthetic(main_self=10))
    b = reports.write(tmp_path / "b.json", reports.synthetic(main_self=110))
    result = run("--table", "frames", "--top", "1", a, b)
    assert result.exit_code == 0
    lines = result.stdout.splitlines()
    assert lines[0] == "frames"
    assert lines[1].startswith("  root:;entry:main  self=+100")
    assert lines[2] == "  ... 1 more"
    assert "... " not in run("--table", "frames", "--top", "0", a, b).stdout


def test_table_option_limits_the_output_and_unknown_name_exits_2(tmp_path: Path) -> None:
    a = reports.write(tmp_path / "a.json", reports.full_report())
    b = reports.write(tmp_path / "b.json", reports.full_report(extra=3))
    both = run(a, b).stdout.splitlines()
    assert "functions" in both and "sleep" in both
    only = run("--table", "sleep", "--table", "timeline", a, b).stdout.splitlines()
    assert [line for line in only if not line.startswith(" ")] == ["sleep", "timeline"]
    result = run("--table", "nope", a, b)
    assert result.exit_code == 2
    assert "Invalid value" in result.stderr


def test_rows_follow_the_sort_field_and_none_counts_as_zero() -> None:
    assert SORT_FIELD["exceptions"] == "self_time_ps"
    result: ReportDiff = {
        "exceptions": {
            "X": {"self_time_ps": 10, "time_ps": 1_000_000},
            "Y": {"self_time_ps": -500, "time_ps": 20},
            "Z": {"self_time_ps": None, "entries": 3},
            "W": {"entries": 99},
        }
    }
    lines = render_diff(result, 10, ())
    assert [line.split()[0] for line in lines] == ["exceptions", "Y", "X", "Z", "W"]
    assert lines[1] == "  Y  self_time_ps=-500 time_ps=+20"
    assert lines[3] == "  Z  self_time_ps=- entries=+3"


def test_values_print_with_thousands_separators() -> None:
    lines = render_diff({"sleep": {"sleep": {"time_ps": 1_234_567, "share": -0.125}}}, 10, ())
    assert lines[1] == "  sleep  time_ps=+1,234,567 share=-0.1250"


def test_json_of_two_files_equals_the_library_diff(tmp_path: Path) -> None:
    left, right = reports.full_report(), reports.full_report(extra=3)
    a = reports.write(tmp_path / "a.json", left)
    b = reports.write(tmp_path / "b.json", right)
    result = run("--json", a, b)
    assert result.exit_code == 0
    assert json.loads(result.stdout) == {"a": str(a), "b": str(b), "diff": diff(left, right)}


def test_directories_list_unmatched_names_and_diff_matched_pairs(tmp_path: Path) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    reports.write(da / "only_a.json", reports.synthetic())
    reports.write(db / "only_b.json", reports.synthetic())
    reports.write(da / "same.json", reports.synthetic())
    reports.write(db / "same.json", reports.synthetic())
    reports.write(da / "moved.json", reports.synthetic(main_self=10))
    reports.write(db / "moved.json", reports.synthetic(main_self=15))
    (db / "skipped.json.part").write_text("{", encoding="utf-8")
    result = run("--table", "functions", da, db)
    assert result.exit_code == 0
    assert result.stdout.splitlines() == [
        "only in A (1): only_a.json",
        "only in B (1): only_b.json",
        "== moved.json ==",
        "functions",
        "  main  self=+5 inclusive=+5",
        "1 reports unchanged",
    ]


def test_directories_against_themselves_count_all_unchanged(tmp_path: Path) -> None:
    reports.write(tmp_path / "a.json", reports.synthetic())
    reports.write(tmp_path / "b.json", reports.synthetic())
    result = run(tmp_path, tmp_path)
    assert result.exit_code == 0
    assert result.stdout == "2 reports unchanged\n"


def test_json_of_two_directories_parses_back_to_the_library_diffs(tmp_path: Path) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    reports.write(da / "x.json", reports.full_report())
    reports.write(db / "x.json", reports.full_report(extra=2))
    reports.write(da / "gone.json", reports.synthetic())
    result = run("--json", da, db)
    assert result.exit_code == 0
    assert json.loads(result.stdout) == {
        "only_a": ["gone.json"],
        "only_b": [],
        "pairs": {"x.json": diff(reports.full_report(), reports.full_report(extra=2))},
    }


def test_file_against_directory_exits_2(tmp_path: Path) -> None:
    file = reports.write(tmp_path / "a.json", reports.synthetic())
    result = run(file, tmp_path)
    assert result.exit_code == 2
    assert "both be files or both be directories" in result.stderr


def test_missing_path_exits_2(tmp_path: Path) -> None:
    file = reports.write(tmp_path / "a.json", reports.synthetic())
    assert run(file, tmp_path / "nope.json").exit_code == 2


def test_bad_report_exits_1_and_prints_nothing(tmp_path: Path) -> None:
    good = reports.write(tmp_path / "a.json", reports.synthetic())
    bad = tmp_path / "b.json"
    bad.write_text("not json", encoding="utf-8")
    result = run(good, bad)
    assert result.exit_code == 1
    assert result.stdout == ""
    assert result.stderr.startswith(f"Error: {bad}")


def test_bad_report_in_a_directory_exits_1_before_any_output(tmp_path: Path) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    for name in ("a.json", "b.json"):
        reports.write(da / name, reports.synthetic())
    reports.write(db / "a.json", reports.synthetic(main_self=20))
    (db / "b.json").write_text("not json", encoding="utf-8")
    result = run(da, db)
    assert result.exit_code == 1
    assert result.stdout == ""
    assert "b.json" in result.stderr


def test_empty_directory_exits_1(tmp_path: Path) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    result = run(da, db)
    assert result.exit_code == 1
    assert "no *.json report" in result.stderr


def sleepy(path: Path) -> Path:
    full = reports.full_report()
    assert full.sleep is not None
    moved = dataclasses.replace(full, sleep=dataclasses.replace(full.sleep, time_ps=1))
    return reports.write(path, moved)


def test_unchanged_pairs_are_counted_after_the_table_filter(tmp_path: Path) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    reports.write(da / "sleepy.json", reports.full_report())
    sleepy(db / "sleepy.json")
    filtered = run("--table", "functions", da, db)
    assert filtered.stdout == "1 reports unchanged\n"
    plain = run(da, db)
    assert plain.stdout.splitlines()[:2] == ["== sleepy.json ==", "sleep"]
    assert plain.stdout.endswith("0 reports unchanged\n")


def test_file_mode_says_when_only_unselected_tables_differ(tmp_path: Path) -> None:
    a = reports.write(tmp_path / "a.json", reports.full_report())
    b = sleepy(tmp_path / "b.json")
    result = run("--table", "functions", a, b)
    assert result.exit_code == 0
    assert result.stdout == "no differences in the selected tables\n"


def test_table_filters_the_json_of_a_file_pair(tmp_path: Path) -> None:
    left, right = reports.full_report(), reports.full_report(extra=3)
    a = reports.write(tmp_path / "a.json", left)
    b = reports.write(tmp_path / "b.json", right)
    result = run("--json", "--table", "functions", a, b)
    assert json.loads(result.stdout)["diff"] == {"functions": diff(left, right)["functions"]}


def test_table_filters_the_json_of_a_directory_pair_and_keeps_empty_pairs(
    tmp_path: Path,
) -> None:
    da, db = tmp_path / "a", tmp_path / "b"
    da.mkdir()
    db.mkdir()
    reports.write(da / "x.json", reports.full_report())
    reports.write(db / "x.json", reports.full_report(extra=2))
    reports.write(da / "sleepy.json", reports.full_report())
    sleepy(db / "sleepy.json")
    result = run("--json", "--table", "functions", da, db)
    expected = diff(reports.full_report(), reports.full_report(extra=2))
    assert json.loads(result.stdout)["pairs"] == {
        "sleepy.json": {},
        "x.json": {"functions": expected["functions"]},
    }


def test_equal_magnitudes_order_by_key() -> None:
    result: ReportDiff = {"functions": {"b": {"self": 5}, "c": {"self": 5}, "a": {"self": -5}}}
    lines = render_diff(result, 10, ())
    assert [line.split()[0] for line in lines[1:]] == ["a", "b", "c"]


def test_json_keys_are_sorted_whatever_the_library_order(tmp_path: Path) -> None:
    left, right = reports.full_report(), reports.full_report(extra=3)
    a = reports.write(tmp_path / "a.json", left)
    b = reports.write(tmp_path / "b.json", right)
    changed = diff(left, right)
    expected = {"a": str(a), "b": str(b), "diff": dict(reversed(changed.items()))}
    result = run("--json", a, b)
    assert result.stdout == json.dumps(expected, indent=2, sort_keys=True) + "\n"


@pytest.mark.parametrize("table", sorted(SORT_FIELD))
def test_every_sort_field_is_a_changed_column(table: str) -> None:
    changed = diff(reports.full_report(), reports.full_report(extra=3))
    assert any(SORT_FIELD[table] in row for row in changed[table].values())


class BrokenStdout:
    def write(self, text: str) -> int:
        raise BrokenPipeError(errno.EPIPE, "broken pipe")

    def flush(self) -> None:
        return None


@pytest.mark.parametrize("json_flag", [[], ["--json"]])
def test_broken_pipe_on_stdout_returns_1_quietly(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
    json_flag: list[str],
) -> None:
    a = reports.write(tmp_path / "a.json", reports.synthetic(main_self=1))
    b = reports.write(tmp_path / "b.json", reports.synthetic(main_self=9))
    monkeypatch.setattr(sys, "stdout", BrokenStdout())
    assert main(["profile", "diff", *json_flag, str(a), str(b)]) == 1
    assert capsys.readouterr().err == ""


def test_full_report_header_agrees_with_its_sections() -> None:
    for extra in (0, 3):
        full = reports.full_report(extra)
        assert full.sleep and full.irq and full.loops and full.hotspots
        assert full.executed == sum(frame.self for frame in full.hotspots.frames)
        assert full.executed == sum(row.self for row in full.hotspots.functions)
        assert full.elapsed_ticks == full.executed + full.sleep.time_ps // 1000
        assert full.elapsed_ps == full.elapsed_ticks * 1000
        assert full.clocks[0].instructions == full.elapsed_ticks
        assert full.clocks[0].ps == full.elapsed_ps
        assert full.sleep.share == full.sleep.time_ps / full.elapsed_ps
        assert full.loops.loops[0].share == full.loops.loops[0].time_ps / full.elapsed_ps
        systick = next(frame for frame in full.hotspots.frames if frame.name == "SysTick")
        row = full.irq.exceptions[0]
        assert row.self_time_ps == systick.self * 1000 == row.time_ps
