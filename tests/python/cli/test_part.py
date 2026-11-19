import io
import subprocess
import sys

import pytest
from click.testing import CliRunner
from popcorn import ConfigError
from popcorn.cli import cli, main
from popcorn.stm32 import __main__ as alias
from popcorn.stm32._explain import explain

PART = "STM32U575QGIx"


def run(*args: str):
    return CliRunner().invoke(cli, ["part", "explain", *args])


def alias_output(capsys: pytest.CaptureFixture[str], *args: str) -> tuple[int, str, str]:
    status = alias.main(["explain", *args])
    captured = capsys.readouterr()
    return status, captured.out, captured.err


def test_cli_output_equals_the_alias_output(capsys: pytest.CaptureFixture[str]) -> None:
    status, expected, _ = alias_output(capsys, PART)
    assert status == 0
    assert expected.splitlines()[0].split()[:3] == ["instance", "ip", "version"]
    result = run(PART)
    assert result.exit_code == 0
    assert result.stdout == expected
    assert result.stderr == ""


def test_trustzone_changes_the_output_and_matches_the_alias(
    capsys: pytest.CaptureFixture[str],
) -> None:
    plain = run(PART).stdout
    secured = run(PART, "--trustzone")
    assert secured.exit_code == 0
    assert secured.stdout != plain
    status, expected, _ = alias_output(capsys, PART, "--trustzone")
    assert status == 0
    assert secured.stdout == expected


def test_unknown_part_exits_1_with_the_config_error_text(
    capsys: pytest.CaptureFixture[str],
) -> None:
    _, _, bare = alias_output(capsys, "NOPE")
    assert bare.strip()
    result = run("NOPE")
    assert result.exit_code == 1
    assert result.stdout == ""
    assert result.stderr == f"Error: {bare}"
    assert main(["part", "explain", "NOPE"]) == 1


def test_alias_prints_the_bare_message_and_returns_1(capsys: pytest.CaptureFixture[str]) -> None:
    status, out, err = alias_output(capsys, "NOPE")
    assert (status, out) == (1, "")
    assert "NOPE" in err
    assert not err.startswith("Error:")


def test_explain_prints_nothing_on_error() -> None:
    out = io.StringIO()
    with pytest.raises(ConfigError, match="NOPE"):
        explain("NOPE", False, out)
    assert out.getvalue() == ""


def test_explain_writes_to_the_given_stream(capsys: pytest.CaptureFixture[str]) -> None:
    out = io.StringIO()
    explain(PART, False, out)
    assert out.getvalue().startswith("instance")
    assert capsys.readouterr().out == ""


def test_part_without_a_part_name_exits_2() -> None:
    assert run().exit_code == 2


def test_alias_does_not_load_click() -> None:
    script = (
        "import sys\n"
        "import popcorn.stm32.__main__\n"
        "sys.exit('click' in sys.modules or 'popcorn.cli' in sys.modules)\n"
    )
    assert subprocess.run([sys.executable, "-c", script], check=False).returncode == 0
