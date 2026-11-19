import errno
import importlib.metadata
import sys
from collections.abc import Iterator

import click
import pytest
from click.testing import CliRunner
from popcorn.cli import cli, main


@pytest.fixture
def probe() -> Iterator[None]:
    @cli.command(name="_probe_fail")
    def fail() -> None:
        raise click.ClickException("x")

    @cli.command(name="_probe_exit")
    def exit_one() -> None:
        click.echo("partial")
        raise click.exceptions.Exit(1)

    @cli.command(name="_probe_pipe")
    def pipe() -> None:
        raise BrokenPipeError

    @cli.command(name="_probe_abort")
    def abort() -> None:
        raise click.exceptions.Abort

    @cli.command(name="_probe_epipe")
    def epipe() -> None:
        raise BrokenPipeError(errno.EPIPE, "broken pipe")

    @cli.command(name="_probe_bool")
    def returns_bool() -> bool:
        return True

    @cli.command(name="_probe_str")
    def returns_str() -> str:
        return "done"

    @cli.command(name="_probe_none")
    def returns_none() -> None:
        return None

    yield
    for name in (
        "_probe_epipe",
        "_probe_bool",
        "_probe_str",
        "_probe_none",
        "_probe_fail",
        "_probe_exit",
        "_probe_pipe",
        "_probe_abort",
    ):
        cli.commands.pop(name, None)


def test_help_through_runner() -> None:
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert result.stdout.startswith("Usage: popcorn")


def test_bare_group_prints_help_and_exits_2() -> None:
    result = CliRunner().invoke(cli, [])
    assert result.exit_code == 2
    assert "Usage: popcorn" in result.output


def test_main_help(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["--help"]) == 0
    assert capsys.readouterr().out.startswith("Usage: popcorn")


def test_main_short_help(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["-h"]) == 0
    assert capsys.readouterr().out.startswith("Usage: popcorn")


def test_main_unknown_command(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["nope"]) == 2
    assert "No such command" in capsys.readouterr().err


def test_main_bare(capsys: pytest.CaptureFixture[str]) -> None:
    assert main([]) == 2
    assert "Usage: popcorn" in capsys.readouterr().err


def test_main_version(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["--version"]) == 0
    expected = f"popcorn, version {importlib.metadata.version('popcorn')}\n"
    assert capsys.readouterr().out == expected


@pytest.mark.usefixtures("probe")
def test_main_click_exception(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["_probe_fail"]) == 1
    assert "Error: x" in capsys.readouterr().err


@pytest.mark.usefixtures("probe")
def test_main_exit_keeps_output(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["_probe_exit"]) == 1
    assert capsys.readouterr().out == "partial\n"


@pytest.mark.usefixtures("probe")
def test_main_abort(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["_probe_abort"]) == 1
    assert "Aborted!" in capsys.readouterr().err


@pytest.mark.usefixtures("probe")
def test_main_broken_pipe(
    capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(sys, "stdout", sys.stdout)
    assert main(["_probe_pipe"]) == 1
    assert capsys.readouterr().err == ""


@pytest.mark.usefixtures("probe")
def test_main_real_epipe_returns(
    capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(sys, "stdout", sys.stdout)
    monkeypatch.setattr(sys, "stderr", sys.stderr)
    assert main(["_probe_epipe"]) == 1
    assert capsys.readouterr().err == ""


@pytest.mark.usefixtures("probe")
@pytest.mark.parametrize("name", ["_probe_bool", "_probe_str", "_probe_none"])
def test_main_non_int_return_is_success(name: str) -> None:
    assert main([name]) == 0
