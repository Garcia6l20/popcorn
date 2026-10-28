"""Command-line options and the `popcorn` marker of the pytest plugin."""

from __future__ import annotations

import enum
from dataclasses import dataclass
from pathlib import Path
from typing import NamedTuple, NoReturn, cast

import pytest

from popcorn.coverage.report import UNMODELLED_MODES, ReportSpec, parse_spec

DEFAULT_CACHE_MAX_AGE_DAYS = 7
MARKER = "popcorn(budget=None, strict=None): popcorn machine settings for this test"


@dataclass(frozen=True, slots=True)
class Settings:
    """The plugin's command-line options, parsed once per session.

    Attributes:
        cov_enabled: Whether every popcorn machine counts coverage, `--popcorn-cov`.
        cov_lcov: The lcov file a bare `lcov` report and the implicit lcov report write, the
            `popcorn_cov_lcov` ini option, resolved against the rootdir.
        cov_reports: The reports written at session end, `--popcorn-cov-report` else
            `popcorn_cov_report`, then the implicit lcov report of `popcorn_cov_lcov`, `[term]`
            when none of them is given. A bare `lcov` report has its target set from `cov_lcov`.
            Command-line targets resolve against the invocation directory, ini targets against
            the rootdir.
        cov_fail_under: The threshold in percent from 0 to 100, `--popcorn-cov-fail-under` else
            `popcorn_cov_fail_under`, None when neither is given. It is validated at startup. The
            session exits with status 1 instead of 0 when the total line coverage is below it.
        cov_include: Glob patterns a source file must match, `--popcorn-cov-include` else
            `popcorn_cov_include`. A command-line value replaces the ini value.
        cov_omit: Glob patterns that drop a source file, `--popcorn-cov-omit` else
            `popcorn_cov_omit`. A command-line value replaces the ini value.
        cov_unmodelled: How lines that access unmodelled registers are reported,
            `--popcorn-cov-unmodelled` else `popcorn_cov_unmodelled`, `hit` when neither is given.
            `hit` counts them as covered and records nothing. `mark` counts them as covered and
            marks them in the `term` and built-in `html` reports. `miss` counts them, their
            functions and their branches as not hit. `mark` and `miss` make every machine record
            the CPU accesses per accessing instruction, which slows each access to a peripheral
            register.
        trace_on_fail: Whether a failing test keeps its trace, `--popcorn-trace-on-fail`.
        trace_dir: Where kept traces go, None for the default location under the pytest cache.
        strict: Whether unmodelled peripheral accesses raise, `--popcorn-strict`.
        budget: The default instruction budget of `run`, `wait_for` and `call`,
            `--popcorn-budget`.
        ring: How many lines of the collapsed trace ring a failure report shows, 0 for none,
            `--popcorn-ring`.
        ring_blocks: Whether every popcorn machine's trace ring keeps BLOCK records,
            `--popcorn-ring-blocks`.
        cache_max_age_days: Days a boot cache entry may go unused before the session-end sweep
            deletes it, 0 for no age limit, the `popcorn_cache_max_age_days` ini option.
    """

    trace_on_fail: bool
    trace_dir: Path | None
    strict: bool
    budget: int | None
    ring: int = 0
    ring_blocks: bool = False
    cache_max_age_days: int = DEFAULT_CACHE_MAX_AGE_DAYS
    cov_enabled: bool = False
    cov_lcov: Path | None = None
    cov_reports: tuple[ReportSpec, ...] = ()
    cov_fail_under: float | None = None
    cov_include: tuple[str, ...] = ()
    cov_omit: tuple[str, ...] = ()
    cov_unmodelled: str = "hit"

    @classmethod
    def from_config(cls, config: pytest.Config) -> Settings:
        """The settings of `config`, relative paths resolved against the invocation directory.

        Raises:
            pytest.UsageError: A coverage option is invalid, or given on the command line
                without `--popcorn-cov`.
        """
        here = config.invocation_params.dir
        trace = config.getoption("popcorn_trace_on_fail")
        budget = config.getoption("popcorn_budget")
        ring = config.getoption("popcorn_ring")
        cov = _coverage_settings(config)
        return cls(
            trace_on_fail=trace is not None,
            trace_dir=_path(trace, here) if trace else None,
            strict=config.getoption("popcorn_strict") is True,
            budget=budget if isinstance(budget, int) else None,
            ring=ring if isinstance(ring, int) else 0,
            ring_blocks=config.getoption("popcorn_ring_blocks") is True,
            cache_max_age_days=_age_days(config.getini("popcorn_cache_max_age_days")),
            cov_enabled=cov.enabled,
            cov_lcov=cov.lcov,
            cov_reports=cov.reports,
            cov_fail_under=cov.fail_under,
            cov_include=cov.include,
            cov_omit=cov.omit,
            cov_unmodelled=cov.unmodelled,
        )


SETTINGS = pytest.StashKey[Settings]()

_COV_DEPENDENT = (
    ("popcorn_cov_report", "--popcorn-cov-report"),
    ("popcorn_cov_fail_under", "--popcorn-cov-fail-under"),
    ("popcorn_cov_include", "--popcorn-cov-include"),
    ("popcorn_cov_omit", "--popcorn-cov-omit"),
    ("popcorn_cov_unmodelled", "--popcorn-cov-unmodelled"),
)


class _Coverage(NamedTuple):
    enabled: bool = False
    lcov: Path | None = None
    reports: tuple[ReportSpec, ...] = ()
    fail_under: float | None = None
    include: tuple[str, ...] = ()
    omit: tuple[str, ...] = ()
    unmodelled: str = "hit"


def _coverage_settings(config: pytest.Config) -> _Coverage:
    enabled = config.getoption("popcorn_cov") is True
    if not enabled:
        for dest, flag in _COV_DEPENDENT:
            if config.getoption(dest) not in (None, []):
                raise pytest.UsageError(f"{flag} needs --popcorn-cov")
        return _Coverage()
    here = config.invocation_params.dir
    root = config.rootpath
    ini_lcov = str(config.getini("popcorn_cov_lcov")).strip()
    lcov = root / ini_lcov if ini_lcov else None
    given = _words(config.getoption("popcorn_cov_report"))
    base = here if given else root
    texts = given or _ini_words(config.getini("popcorn_cov_report"))
    specs: list[ReportSpec] = []
    for text in texts:
        try:
            spec = parse_spec(text, base)
        except ValueError as error:
            raise pytest.UsageError(f"--popcorn-cov-report: {error}") from error
        if spec.kind == "lcov" and spec.target is None:
            if lcov is None:
                raise pytest.UsageError(
                    "--popcorn-cov-report=lcov needs a path: lcov:PATH or the popcorn_cov_lcov "
                    "ini option"
                )
            spec = ReportSpec("lcov", lcov)
        if spec.kind == "gcovr" and spec.target is None:
            raise pytest.UsageError("--popcorn-cov-report=gcovr needs a path: gcovr:PATH")
        if spec not in specs:
            specs.append(spec)
    if lcov is not None and ReportSpec("lcov", lcov) not in specs:
        specs.insert(0, ReportSpec("lcov", lcov))
    if not specs:
        specs.append(ReportSpec("term"))
    return _Coverage(
        True,
        lcov,
        tuple(specs),
        _fail_under(
            config.getoption("popcorn_cov_fail_under"), config.getini("popcorn_cov_fail_under")
        ),
        _patterns(config, "popcorn_cov_include"),
        _patterns(config, "popcorn_cov_omit"),
        _unmodelled(
            config.getoption("popcorn_cov_unmodelled"), config.getini("popcorn_cov_unmodelled")
        ),
    )


def _unmodelled(option: object, ini: object) -> str:
    text = str(ini).strip() if option is None else str(option)
    if not text:
        return "hit"
    if text not in UNMODELLED_MODES:
        raise pytest.UsageError(
            f"popcorn_cov_unmodelled must be one of {', '.join(UNMODELLED_MODES)}, not {text!r}"
        )
    return text


def _words(value: object) -> tuple[str, ...]:
    if not isinstance(value, list):
        return ()
    return tuple(str(word) for word in cast(list[object], value))


def _ini_words(value: object) -> tuple[str, ...]:
    return tuple(line.strip() for line in _words(value) if line.strip())


def _patterns(config: pytest.Config, name: str) -> tuple[str, ...]:
    return _words(config.getoption(name)) or _words(config.getini(name))


def _fail_under(option: object, ini: object) -> float | None:
    if option is None:
        text = str(ini).strip()
        if not text:
            return None
        try:
            value = float(text)
        except ValueError:
            raise pytest.UsageError(
                f"popcorn_cov_fail_under must be a number from 0 to 100, not {text!r}"
            ) from None
    else:
        value = cast(float, option)
    if not 0 <= value <= 100:
        raise pytest.UsageError(f"the coverage threshold must be from 0 to 100, not {value}")
    return value


def _path(value: object, base: Path) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    return base / value


def _age_days(value: object) -> int:
    text = str(value).strip()
    if not text:
        return DEFAULT_CACHE_MAX_AGE_DAYS
    try:
        days = int(text)
    except ValueError:
        days = -1
    if days < 0:
        raise pytest.UsageError(
            f"popcorn_cache_max_age_days must be a whole number of days, 0 or above, not {text!r}"
        )
    return days


def pytest_addoption(parser: pytest.Parser) -> None:
    """Register the `--popcorn-*` options and the `popcorn_*` ini options."""
    parser.addini(
        "popcorn_cache_max_age_days",
        help="days a boot cache entry may go unused before the session-end sweep deletes it, "
        "0 for no age limit, default 7",
        default=str(DEFAULT_CACHE_MAX_AGE_DAYS),
    )
    parser.addini("popcorn_cov_lcov", help="the lcov file written by --popcorn-cov", default="")
    parser.addini(
        "popcorn_cov_report",
        help="reports written by --popcorn-cov, one TYPE[:TARGET] per line",
        type="linelist",
    )
    parser.addini(
        "popcorn_cov_fail_under",
        help="exit with status 1 instead of 0 when the total line coverage is below PERCENT, "
        "from 0 to 100",
        default="",
    )
    parser.addini(
        "popcorn_cov_include",
        help="glob patterns, one per line: only matching source files are reported",
        type="linelist",
    )
    parser.addini(
        "popcorn_cov_omit",
        help="glob patterns, one per line: matching source files are not reported",
        type="linelist",
    )
    parser.addini(
        "popcorn_cov_unmodelled",
        help="mark, miss or hit: how --popcorn-cov reports lines that access unmodelled "
        "registers, default hit",
        default="",
    )
    group = parser.getgroup("popcorn", "popcorn firmware emulation")
    group.addoption(
        "--popcorn-cov",
        action="store_true",
        help="count firmware coverage of every popcorn machine, merged across tests and "
        "workers. The reports come from --popcorn-cov-report, for an lcov file "
        "--popcorn-cov-report=lcov:PATH or the popcorn_cov_lcov ini option. With none "
        "of them set the report is the terminal table, term",
    )
    group.addoption(
        "--popcorn-cov-report",
        action="append",
        metavar="TYPE[:TARGET]",
        help="a report written at session end: term, html[:DIR], xml[:FILE], lcov[:FILE] or "
        "gcovr:FILE. "
        "Repeat the option for several. Replaces the popcorn_cov_report ini option",
    )
    group.addoption(
        "--popcorn-cov-fail-under",
        type=float,
        metavar="PERCENT",
        help="exit with status 1 instead of 0 when the total line coverage is below PERCENT, "
        "from 0 to 100",
    )
    group.addoption(
        "--popcorn-cov-include",
        action="append",
        metavar="GLOB",
        help="report only source files matching GLOB, repeatable. Replaces the "
        "popcorn_cov_include ini option",
    )
    group.addoption(
        "--popcorn-cov-omit",
        action="append",
        metavar="GLOB",
        help="leave out source files matching GLOB, repeatable. Replaces the popcorn_cov_omit "
        "ini option",
    )
    group.addoption(
        "--popcorn-cov-unmodelled",
        choices=UNMODELLED_MODES,
        metavar="MODE",
        help="how lines that access unmodelled registers are reported: hit counts them as "
        "covered and records nothing (default), mark counts them as covered and marks them in "
        "the term and built-in html reports, miss counts them as not hit. mark and miss slow "
        "every access to a peripheral register. Replaces the popcorn_cov_unmodelled ini option",
    )
    group.addoption(
        "--popcorn-trace-on-fail",
        nargs="?",
        const="",
        default=None,
        metavar="DIR",
        help="keep a .pctr trace of each failing test using a popcorn machine fixture, "
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
        "groups collapsed, default 0: not shown. The ring holds no BLOCK record without "
        "--popcorn-ring-blocks",
    )
    group.addoption(
        "--popcorn-ring-blocks",
        action="store_true",
        help="every popcorn machine's trace ring keeps a BLOCK record per executed block. The "
        "ring keeps 4096 records: a tight loop fills it and pushes out older MMIO records, "
        "the report collapses the repeats. Shown in a failure report with --popcorn-ring=N",
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

_KEYWORDS = ("budget", "strict")

CONFIG = (
    "config is not a marker keyword: set it on the machine fixture, "
    "@popcorn.pytest.machine(config=...), or pass it to machine_factory(description, config=...)"
)


@dataclass(frozen=True, slots=True)
class MarkerOptions:
    """The keywords of the closest `popcorn` marker of a test, `UNSET` where absent.

    Attributes:
        budget: The default instruction budget, None for unbounded runs.
        strict: None or absent, the command line then the machine description; True or False
            overrides.
    """

    budget: int | Unset | None = UNSET
    strict: bool | None = None


def marker_options(item: pytest.Item) -> MarkerOptions:
    """The checked keywords of the closest `popcorn` marker of `item`: the function's, else its
    class's, else its module's. Keywords do not merge across levels.

    Raises:
        pytest.fail.Exception: A positional argument, `config`, an unknown keyword or a value of
            the wrong type or range.
    """
    marker = item.get_closest_marker("popcorn")
    if marker is None:
        return MarkerOptions()
    if marker.args:
        _refuse(f"takes keywords only, not the positional {marker.args!r}")
    if "config" in marker.kwargs:
        _refuse(CONFIG)
    unknown = sorted(set(marker.kwargs) - set(_KEYWORDS))
    if unknown:
        _refuse(f"has no keyword {', '.join(unknown)}: use {', '.join(_KEYWORDS)}")
    budget: object = marker.kwargs.get("budget", UNSET)
    strict: object = marker.kwargs.get("strict")
    if not (
        budget is UNSET
        or budget is None
        or (isinstance(budget, int) and not isinstance(budget, bool) and budget > 0)
    ):
        _refuse(f"budget is an int above 0 or None, not {budget!r}")
    if not (strict is None or isinstance(strict, bool)):
        _refuse(f"strict is a bool or None, not {strict!r}")
    return MarkerOptions(budget=cast(int | Unset | None, budget), strict=strict)


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item: pytest.Item) -> None:
    """Check the `popcorn` marker of every marked test, whatever fixtures it uses."""
    if item.get_closest_marker("popcorn") is not None:
        marker_options(item)


def _refuse(message: str) -> NoReturn:
    pytest.fail(f"@pytest.mark.popcorn {message}", pytrace=False)
