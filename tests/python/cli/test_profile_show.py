from pathlib import Path

import reports
from click.testing import CliRunner
from popcorn.cli import cli


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["profile", "show", *map(str, args)])


def test_one_file_prints_the_report_text(tmp_path: Path) -> None:
    report = reports.synthetic()
    path = reports.write(tmp_path / "a.json", report)
    result = run(path)
    assert result.exit_code == 0
    assert result.stdout == report.text() + "\n"


def test_directory_prints_one_header_per_report_sorted_and_skips_part_files(
    tmp_path: Path,
) -> None:
    reports.write(tmp_path / "b.json", reports.synthetic(main_self=20))
    reports.write(tmp_path / "a.json", reports.synthetic(main_self=10))
    (tmp_path / "c.json.part").write_text("{", encoding="utf-8")
    result = run(tmp_path)
    assert result.exit_code == 0
    lines = result.stdout.splitlines()
    first, second = f"== {tmp_path / 'a.json'} ==", f"== {tmp_path / 'b.json'} =="
    assert lines.index(first) < lines.index(second)
    assert lines.count(first) == lines.count(second) == 1
    assert "c.json" not in result.stdout


def test_missing_path_among_good_ones_exits_1_and_prints_the_good_ones(tmp_path: Path) -> None:
    report = reports.synthetic()
    good = reports.write(tmp_path / "a.json", report)
    result = run(tmp_path / "nope.json", good)
    assert result.exit_code == 1
    assert report.text() in result.stdout
    assert f"Error: {tmp_path / 'nope.json'}" in result.stderr


def test_malformed_file_exits_1_and_does_not_stop_the_others(tmp_path: Path) -> None:
    bad = tmp_path / "a.json"
    bad.write_text("not json", encoding="utf-8")
    good = reports.write(tmp_path / "b.json", reports.synthetic())
    result = run(bad, good)
    assert result.exit_code == 1
    assert f"Error: {bad}" in result.stderr
    assert "== " + str(good) + " ==" in result.stdout


def test_empty_directory_exits_1(tmp_path: Path) -> None:
    result = run(tmp_path)
    assert result.exit_code == 1
    assert "no *.json report" in result.stderr


def test_no_path_is_a_usage_error() -> None:
    assert run().exit_code == 2


def test_directory_of_one_report_has_no_header(tmp_path: Path) -> None:
    report = reports.synthetic()
    reports.write(tmp_path / "a.json", report)
    result = run(tmp_path)
    assert result.exit_code == 0
    assert result.stdout == report.text() + "\n"


def test_one_file_plus_a_missing_path_has_a_header(tmp_path: Path) -> None:
    good = reports.write(tmp_path / "a.json", reports.synthetic())
    result = run(good, tmp_path / "nope.json")
    assert result.exit_code == 1
    assert f"== {good} ==" in result.stdout.splitlines()
