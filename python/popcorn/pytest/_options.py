"""Command-line options and the `popcorn` marker of the pytest plugin."""

from __future__ import annotations

import enum
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, cast

import pytest

MARKER = "popcorn(config=None, budget=None, strict=None): popcorn machine settings for this test"


@dataclass(frozen=True, slots=True)
class Settings:
    """The plugin's command-line options, parsed once per session.

    Attributes:
        config: The config of `popcorn_project` applied when a test names none,
            `--popcorn-config`.
        cov: The lcov file written at session end, `--popcorn-cov`.
        trace_on_fail: Whether a failing test keeps its trace, `--popcorn-trace-on-fail`.
        trace_dir: Where kept traces go, None for the default location under the pytest cache.
        strict: Whether unmodelled peripheral accesses raise, `--popcorn-strict`.
        budget: The default instruction budget of `run`, `wait_for` and `call`,
            `--popcorn-budget`.
        ring: How many lines of the collapsed trace ring a failure report shows, 0 for none,
            `--popcorn-ring`.
    """

    config: str | None
    cov: Path | None
    trace_on_fail: bool
    trace_dir: Path | None
    strict: bool
    budget: int | None
    ring: int = 0

    @classmethod
    def from_config(cls, config: pytest.Config) -> Settings:
        """The settings of `config`, relative paths resolved against the invocation directory."""
        here = config.invocation_params.dir
        cov = _path(config.getoption("popcorn_cov"), here)
        trace = config.getoption("popcorn_trace_on_fail")
        name = config.getoption("popcorn_config")
        budget = config.getoption("popcorn_budget")
        ring = config.getoption("popcorn_ring")
        return cls(
            config=name if isinstance(name, str) else None,
            cov=cov,
            trace_on_fail=trace is not None,
            trace_dir=_path(trace, here) if trace else None,
            strict=config.getoption("popcorn_strict") is True,
            budget=budget if isinstance(budget, int) else None,
            ring=ring if isinstance(ring, int) else 0,
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
        "--popcorn-config",
        metavar="NAME",
        help="config NAME of popcorn_project applied when a test names none",
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
        "in .pytest_cache/d/popcorn/traces, or in DIR given as --popcorn-trace-on-fail=DIR: "
        "a bare flag before a path takes the path as DIR",
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
    group.addoption(
        "--popcorn-ring",
        type=int,
        default=0,
        metavar="N",
        help="show the last N lines of the trace ring in a failure report, repeated record "
        "groups collapsed, default 0: not shown",
    )


def pytest_configure(config: pytest.Config) -> None:
    """Register the `popcorn` marker and parse the options.

    Raises:
        pytest.UsageError: `--popcorn-budget` is not above 0, or `--popcorn-ring` is below 0.
    """
    config.addinivalue_line("markers", MARKER)
    settings = Settings.from_config(config)
    if settings.budget is not None and settings.budget <= 0:
        raise pytest.UsageError(f"--popcorn-budget must be above 0, not {settings.budget}")
    if settings.ring < 0:
        raise pytest.UsageError(f"--popcorn-ring must be 0 or above, not {settings.ring}")
    config.stash[SETTINGS] = settings


class Unset(enum.Enum):
    """A marker keyword that was not given."""

    UNSET = 0


UNSET = Unset.UNSET

_KEYWORDS = ("config", "budget", "strict")


@dataclass(frozen=True, slots=True)
class MarkerOptions:
    """The keywords of the closest `popcorn` marker of a test, `UNSET` where absent.

    Attributes:
        config: The config to apply, None for the base machine description.
        budget: The default instruction budget, None for unbounded runs.
        strict: None or absent, the command line then the machine description; True or False
            overrides.
    """

    config: str | Unset | None = UNSET
    budget: int | Unset | None = UNSET
    strict: bool | None = None


def marker_options(item: pytest.Item) -> MarkerOptions:
    """The checked keywords of the closest `popcorn` marker of `item`: the function's, else its
    class's, else its module's. Keywords do not merge across levels.

    Raises:
        pytest.fail.Exception: A positional argument, an unknown keyword or a value of the wrong
            type or range.
    """
    marker = item.get_closest_marker("popcorn")
    if marker is None:
        return MarkerOptions()
    if marker.args:
        _refuse(f"takes keywords only, not the positional {marker.args!r}")
    unknown = sorted(set(marker.kwargs) - set(_KEYWORDS))
    if unknown:
        _refuse(f"has no keyword {', '.join(unknown)}: use {', '.join(_KEYWORDS)}")
    config: object = marker.kwargs.get("config", UNSET)
    budget: object = marker.kwargs.get("budget", UNSET)
    strict: object = marker.kwargs.get("strict")
    if not (config is UNSET or config is None or isinstance(config, str)):
        _refuse(f"config is a str or None, not {config!r}")
    if not (
        budget is UNSET
        or budget is None
        or (isinstance(budget, int) and not isinstance(budget, bool) and budget > 0)
    ):
        _refuse(f"budget is an int above 0 or None, not {budget!r}")
    if not (strict is None or isinstance(strict, bool)):
        _refuse(f"strict is a bool or None, not {strict!r}")
    return MarkerOptions(
        config=cast(str | Unset | None, config),
        budget=cast(int | Unset | None, budget),
        strict=strict,
    )


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item: pytest.Item) -> None:
    """Check the `popcorn` marker of every marked test, whatever fixtures it uses."""
    if item.get_closest_marker("popcorn") is not None:
        marker_options(item)


def _refuse(message: str) -> NoReturn:
    pytest.fail(f"@pytest.mark.popcorn {message}", pytrace=False)
