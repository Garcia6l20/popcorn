import errno
import sys
from pathlib import Path

import pytest
import reports
from click.testing import CliRunner
from popcorn.cli import cli, main


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["profile", "folded", *map(str, args)])


def test_output_file_holds_the_folded_lines(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic())
    out = tmp_path / "a.folded"
    result = run(source, "-o", out)
    assert result.exit_code == 0
    assert out.read_text(encoding="utf-8").splitlines() == reports.FOLDED
    assert "3 lines" in result.stderr


def test_stdout_equals_the_file_and_dash_equals_no_option(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic())
    bare = run(source)
    dash = run(source, "-o", "-")
    assert bare.exit_code == dash.exit_code == 0
    assert bare.stdout.splitlines() == reports.FOLDED
    assert dash.stdout == bare.stdout


def test_report_without_hotspots_exits_1(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic(hotspots=False))
    result = run(source)
    assert result.exit_code == 1
    assert "hotspots" in result.stderr
    assert result.stderr.startswith("Error:")


def test_directory_argument_is_a_usage_error(tmp_path: Path) -> None:
    assert run(tmp_path).exit_code == 2


def test_missing_report_is_a_usage_error(tmp_path: Path) -> None:
    assert run(tmp_path / "nope.json").exit_code == 2


def test_output_directory_is_a_usage_error(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic())
    result = run(source, "-o", tmp_path)
    assert result.exit_code == 2
    assert "Invalid value" in result.stderr


def test_output_under_a_missing_parent_exits_1(tmp_path: Path) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic())
    out = tmp_path / "missing" / "a.folded"
    result = run(source, "-o", out)
    assert result.exit_code == 1
    assert result.stderr.startswith("Error:")
    assert not out.exists()


class BrokenStdout:
    def write(self, text: str) -> int:
        raise BrokenPipeError(errno.EPIPE, "broken pipe")

    def flush(self) -> None:
        return None


def test_broken_pipe_on_stdout_returns_1_quietly(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    source = reports.write(tmp_path / "a.json", reports.synthetic())
    monkeypatch.setattr(sys, "stdout", BrokenStdout())
    monkeypatch.setattr(sys, "stderr", sys.stderr)
    assert main(["profile", "folded", str(source)]) == 1
    assert capsys.readouterr().err == ""


def test_malformed_report_exits_1(tmp_path: Path) -> None:
    source = tmp_path / "a.json"
    source.write_text("[]", encoding="utf-8")
    result = run(source)
    assert result.exit_code == 1
    assert f"Error: {source}" in result.stderr
