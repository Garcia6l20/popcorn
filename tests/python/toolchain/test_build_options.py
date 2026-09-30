import tomllib
from pathlib import Path

import pytest

from popcorn_build.check import stages
from popcorn_build.options import parse_switch, switch

PYPROJECT = Path(__file__).resolve().parents[3] / "pyproject.toml"


@pytest.mark.parametrize(
    ("raw", "expected"),
    [("ON", True), ("on", True), ("1", True), ("OFF", False), ("Off", False), ("0", False)],
)
def test_a_switch_takes_on_off_1_or_0_in_any_case(raw: str, expected: bool) -> None:
    assert parse_switch("POPCORN_BUILD_TESTS", raw) is expected


@pytest.mark.parametrize("raw", ["yes", "", "2", "true"])
def test_any_other_switch_value_stops_the_configure(raw: str) -> None:
    with pytest.raises(SystemExit, match=r"POPCORN_BUILD_TESTS=.*expected ON, OFF, 1 or 0"):
        parse_switch("POPCORN_BUILD_TESTS", raw)


def test_the_package_build_turns_tests_and_examples_off() -> None:
    variables = tomllib.loads(PYPROJECT.read_text())["tool"]["pcons"]["variables"]
    assert variables == {"POPCORN_BUILD_TESTS": "OFF", "POPCORN_BUILD_EXAMPLES": "OFF"}


@pytest.mark.parametrize("raw", [True, False])
def test_a_toml_bool_is_refused_with_the_switch_name(raw: bool) -> None:
    with pytest.raises(SystemExit, match=r"POPCORN_BUILD_TESTS=.*expected ON, OFF, 1 or 0"):
        parse_switch("POPCORN_BUILD_TESTS", raw)


def test_an_unset_switch_is_on(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("PCONS_VARS", raising=False)
    monkeypatch.delenv("POPCORN_BUILD_TESTS", raising=False)
    assert switch("POPCORN_BUILD_TESTS", "tests") is True


@pytest.mark.parametrize("asan", [False, True])
def test_the_gate_builds_with_tests_and_examples_on(asan: bool) -> None:
    build = dict(stages(asan))["build"]
    assert "POPCORN_BUILD_TESTS=ON" in build
    assert "POPCORN_BUILD_EXAMPLES=ON" in build
