"""Command-line options and the `popcorn` marker of the pytest plugin."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import pytest

MARKER = "popcorn(config=None, budget=None, strict=None): popcorn machine settings for this test"


@dataclass(frozen=True, slots=True)
class Settings:
    """The plugin's command-line options, parsed once per session.

    Attributes:
        project: The project file, `--popcorn-project` or `<rootdir>/popcorn.toml`.
        config: The `[configs.NAME]` applied when a test names none, `--popcorn-config`.
        cov: The lcov file written at session end, `--popcorn-cov`.
        trace_on_fail: Whether a failing test keeps its trace, `--popcorn-trace-on-fail`.
        trace_dir: Where kept traces go, None for the default location under the pytest cache.
        strict: Whether unmodelled peripheral accesses raise, `--popcorn-strict`.
        budget: The default instruction budget of `run`, `wait_for` and `call`,
            `--popcorn-budget`.
    """

    project: Path
    config: str | None
    cov: Path | None
    trace_on_fail: bool
    trace_dir: Path | None
    strict: bool
    budget: int | None

    @classmethod
    def from_config(cls, config: pytest.Config) -> Settings:
        """The settings of `config`, relative paths resolved against the invocation directory."""
        here = config.invocation_params.dir
        project = _path(config.getoption("popcorn_project"), here)
        cov = _path(config.getoption("popcorn_cov"), here)
        trace = config.getoption("popcorn_trace_on_fail")
        name = config.getoption("popcorn_config")
        budget = config.getoption("popcorn_budget")
        return cls(
            project=project if project is not None else config.rootpath / "popcorn.toml",
            config=name if isinstance(name, str) else None,
            cov=cov,
            trace_on_fail=trace is not None,
            trace_dir=_path(trace, here) if trace else None,
            strict=config.getoption("popcorn_strict") is True,
            budget=budget if isinstance(budget, int) else None,
        )


SETTINGS = pytest.StashKey[Settings]()


def _path(value: object, base: Path) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    return base / value


def pytest_addoption(parser: pytest.Parser) -> None:
    """Register the six `--popcorn-*` options."""
    group = parser.getgroup("popcorn", "popcorn firmware emulation")
    group.addoption(
        "--popcorn-project",
        metavar="PATH",
        help="project file, default <rootdir>/popcorn.toml",
    )
    group.addoption(
        "--popcorn-config",
        metavar="NAME",
        help="[configs.NAME] of the project file applied when a test names none",
    )
    group.addoption(
        "--popcorn-cov",
        metavar="LCOV",
        help="write firmware coverage merged across tests and workers as lcov",
    )
    group.addoption(
        "--popcorn-trace-on-fail",
        nargs="?",
        const="",
        default=None,
        metavar="DIR",
        help="keep a .pctr trace of each failing test using the machine fixture, "
        "in .pytest_cache/d/popcorn/traces, or in DIR given as --popcorn-trace-on-fail=DIR",
    )
    group.addoption(
        "--popcorn-strict",
        action="store_true",
        help="unmodelled peripheral accesses raise ModelError",
    )
    group.addoption(
        "--popcorn-budget",
        type=int,
        metavar="N",
        help="default instruction budget of run, wait_for and call",
    )


def pytest_configure(config: pytest.Config) -> None:
    """Register the `popcorn` marker and parse the options."""
    config.addinivalue_line("markers", MARKER)
    config.stash[SETTINGS] = Settings.from_config(config)
