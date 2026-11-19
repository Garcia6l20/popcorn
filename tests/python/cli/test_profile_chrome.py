import dataclasses
import json
from pathlib import Path

import reports
from click.testing import CliRunner
from popcorn.cli import cli
from popcorn.cli._cpuprofile import to_cpuprofile


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["profile", "chrome", *map(str, args)])


def test_output_equals_the_conversion(tmp_path: Path) -> None:
    report = reports.tree_report()
    source = reports.write(tmp_path / "a.json", report)
    out = tmp_path / "a.cpuprofile"
    result = run(source, "-o", out)
    assert result.exit_code == 0
    assert json.loads(out.read_text(encoding="utf-8")) == to_cpuprofile(report)


def test_no_hotspots_exits_1_naming_the_analysis(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic(hotspots=False))
    result = run(source, "-o", tmp_path / "x.cpuprofile")
    assert result.exit_code == 1
    assert "hotspots" in result.stderr
    assert str(source) in result.stderr


def test_malformed_tree_exits_1_naming_the_file(tmp_path: Path) -> None:
    frames = reports.tree_frames()
    broken = (*frames[:2], dataclasses.replace(frames[2], parent=5), *frames[3:])
    source = reports.write(tmp_path / "a.json", reports.tree_report(broken))
    result = run(source, "-o", tmp_path / "x.cpuprofile")
    assert result.exit_code == 1
    assert result.stderr.startswith("Error:")
    assert str(source) in result.stderr


def test_output_directory_is_a_usage_error(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.tree_report())
    assert run(source, "-o", tmp_path).exit_code == 2


def test_output_under_a_missing_parent_exits_1(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.tree_report())
    result = run(source, "-o", tmp_path / "missing" / "x.cpuprofile")
    assert result.exit_code == 1
    assert result.stderr.startswith("Error:")


def test_missing_output_option_is_a_usage_error(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.tree_report())
    assert run(source).exit_code == 2


def test_non_report_file_exits_1(tmp_path: Path) -> None:
    source = tmp_path / "a.json"
    source.write_text("[]", encoding="utf-8")
    result = run(source, "-o", tmp_path / "x.cpuprofile")
    assert result.exit_code == 1
    assert str(source) in result.stderr
