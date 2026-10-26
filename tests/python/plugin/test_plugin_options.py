"""The plugin's options and marker, FR-PYT-5: registered and parsed."""

from __future__ import annotations

import textwrap

import pytest

from .helpers import write_conftest

pytest_plugins = ["pytester"]

OPTIONS = (
    "--popcorn-cov",
    "--popcorn-cov-report=TYPE[[]:TARGET]",
    "--popcorn-cov-fail-under=PERCENT",
    "--popcorn-cov-include=GLOB",
    "--popcorn-cov-omit=GLOB",
    "--popcorn-cov-unmodelled=MODE",
    "--popcorn-trace-on-fail=[[]DIR]",
    "--popcorn-strict",
    "--popcorn-budget=N",
    "--popcorn-ring=N",
    "--popcorn-ring-blocks",
)


def test_help_lists_the_options(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("-p", "no:asyncio", "--help")
    assert result.ret == 0
    result.stdout.fnmatch_lines(["popcorn firmware emulation:", *(f"*{o}*" for o in OPTIONS)])


def test_markers_lists_popcorn(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("-p", "no:asyncio", "--markers")
    result.stdout.fnmatch_lines(["@pytest.mark.popcorn(budget=None, strict=None):*"])


def test_defaults_parse_to_settings(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        from popcorn.pytest import Settings
        from popcorn.pytest._options import SETTINGS

        def test_defaults(request):
            settings = request.config.stash[SETTINGS]
            assert settings == Settings(
                trace_on_fail=False,
                trace_dir=None,
                strict=False,
                budget=None,
                ring=0,
                ring_blocks=False,
            )
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=1)


def test_values_parse_relative_to_the_invocation_directory(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    here = pytester.mkdir("here")
    (here / "test_values.py").write_text(textwrap.dedent(f"""
        from pathlib import Path

        from popcorn.coverage.report import ReportSpec
        from popcorn.pytest._options import SETTINGS

        def test_values(request):
            settings = request.config.stash[SETTINGS]
            here = Path({str(here)!r})
            assert request.config.rootpath == here.parent
            assert settings.cov_enabled
            assert settings.cov_reports == (ReportSpec("lcov", here / "out" / "cov.info"),)
            assert settings.trace_on_fail
            assert settings.trace_dir == here / "traces"
            assert settings.strict
            assert settings.budget == 1000
            assert settings.ring_blocks
        """))
    monkeypatch.chdir(here)
    result = pytester.runpytest(
        "-p",
        "no:asyncio",
        "--popcorn-cov",
        "--popcorn-cov-report=lcov:out/cov.info",
        "--popcorn-trace-on-fail=traces",
        "--popcorn-strict",
        "--popcorn-budget=1000",
        "--popcorn-ring-blocks",
    )
    result.assert_outcomes(passed=1)


@pytest.mark.parametrize("option", ["--popcorn-project=popcorn.toml", "--popcorn-config=variant"])
def test_removed_options_are_usage_errors(pytester: pytest.Pytester, option: str) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("-p", "no:asyncio", option)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([f"*unrecognized arguments: {option}*"])


def test_bare_trace_on_fail_takes_the_default_directory(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        from popcorn.pytest._options import SETTINGS

        def test_bare(request):
            settings = request.config.stash[SETTINGS]
            assert settings.trace_on_fail
            assert settings.trace_dir is None
        """)
    pytester.runpytest("-p", "no:asyncio", "--popcorn-trace-on-fail").assert_outcomes(passed=1)
