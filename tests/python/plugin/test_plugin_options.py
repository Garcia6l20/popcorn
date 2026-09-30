"""The plugin's options and marker, FR-PYT-5: registered and parsed."""

from __future__ import annotations

import textwrap

import pytest

from .helpers import write_conftest

pytest_plugins = ["pytester"]

OPTIONS = (
    "--popcorn-project=PATH",
    "--popcorn-config=NAME",
    "--popcorn-cov=LCOV",
    "--popcorn-trace-on-fail=[[]DIR]",
    "--popcorn-strict",
    "--popcorn-budget=N",
)


def test_help_lists_the_six_options(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("--help")
    assert result.ret == 0
    result.stdout.fnmatch_lines(["popcorn firmware emulation:", *(f"*{o}*" for o in OPTIONS)])


def test_markers_lists_popcorn(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("--markers")
    result.stdout.fnmatch_lines(["@pytest.mark.popcorn(config=None, budget=None, strict=None):*"])


def test_defaults_parse_to_settings(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        from popcorn.pytest import Settings
        from popcorn.pytest._options import SETTINGS

        def test_defaults(request):
            settings = request.config.stash[SETTINGS]
            assert settings == Settings(
                project=request.config.rootpath / "popcorn.toml",
                config=None,
                cov=None,
                trace_on_fail=False,
                trace_dir=None,
                strict=False,
                budget=None,
            )
        """)
    pytester.runpytest().assert_outcomes(passed=1)


def test_values_parse_relative_to_the_invocation_directory(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    here = pytester.mkdir("here")
    (here / "test_values.py").write_text(textwrap.dedent(f"""
        from pathlib import Path

        from popcorn.pytest._options import SETTINGS

        def test_values(request):
            settings = request.config.stash[SETTINGS]
            here = Path({str(here)!r})
            assert request.config.rootpath == here.parent
            assert settings.project == here / "sub" / "p.toml"
            assert settings.config == "variant"
            assert settings.cov == here / "out" / "cov.info"
            assert settings.trace_on_fail
            assert settings.trace_dir == here / "traces"
            assert settings.strict
            assert settings.budget == 1000
        """))
    monkeypatch.chdir(here)
    result = pytester.runpytest(
        "--popcorn-project=sub/p.toml",
        "--popcorn-config=variant",
        "--popcorn-cov=out/cov.info",
        "--popcorn-trace-on-fail=traces",
        "--popcorn-strict",
        "--popcorn-budget=1000",
    )
    result.assert_outcomes(passed=1)


def test_the_default_project_is_at_the_rootdir(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    here = pytester.mkdir("here")
    (here / "test_default.py").write_text(textwrap.dedent(f"""
        from pathlib import Path

        from popcorn.pytest._options import SETTINGS

        def test_default_project(request):
            project = request.config.stash[SETTINGS].project
            assert project == Path({str(pytester.path)!r}) / "popcorn.toml"
        """))
    monkeypatch.chdir(here)
    pytester.runpytest().assert_outcomes(passed=1)


def test_bare_trace_on_fail_takes_the_default_directory(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        from popcorn.pytest._options import SETTINGS

        def test_bare(request):
            settings = request.config.stash[SETTINGS]
            assert settings.trace_on_fail
            assert settings.trace_dir is None
        """)
    pytester.runpytest("--popcorn-trace-on-fail").assert_outcomes(passed=1)
