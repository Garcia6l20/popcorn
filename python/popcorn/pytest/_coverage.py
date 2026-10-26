"""`--popcorn-cov`: one `.pcov` file per process, merged into one lcov file at session end."""

from __future__ import annotations

import shutil
import tempfile
from decimal import Decimal
from pathlib import Path
from typing import cast

import pytest

from popcorn.coverage import merge_session
from popcorn.coverage.report import Totals, below, generate, truncated_percent
from popcorn.pytest._options import SETTINGS
from popcorn.pytest._session import COVERAGE, SESSION, WORKER_SUMMARIES, CoverageRun, summary

OUTCOME = pytest.StashKey[str]()
TERM = pytest.StashKey[str]()


class XdistHooks:
    """The controller's side of xdist: coverage directory out, boot counters in."""

    def __init__(self, config: pytest.Config) -> None:
        """Serve the workers of `config`."""
        self._config = config

    @pytest.hookimpl(optionalhook=True)
    def pytest_configure_node(self, node: object) -> None:
        """Hand the coverage directory to a new worker."""
        run = self._config.stash.get(COVERAGE, None)
        if run is not None:
            workerinput: object = getattr(node, "workerinput", None)
            if isinstance(workerinput, dict):
                cast(dict[str, object], workerinput)["popcorn_cov_dir"] = str(run.directory)

    @pytest.hookimpl(optionalhook=True)
    def pytest_testnodedown(self, node: object, error: object) -> None:
        """Keep the boot counters of a worker that finished."""
        output: object = getattr(node, "workeroutput", None)
        if not isinstance(output, dict):
            return
        counters: object = cast(dict[str, object], output).get("popcorn")
        if isinstance(counters, dict):
            self._config.stash.setdefault(WORKER_SUMMARIES, list[dict[str, object]]()).append(
                cast(dict[str, object], counters)
            )


@pytest.hookimpl(trylast=True)
def pytest_configure(config: pytest.Config) -> None:
    """Set up this process's coverage run, and the xdist hooks on the controller."""
    workerinput: object = getattr(config, "workerinput", None)
    if isinstance(workerinput, dict):
        received = cast(dict[str, object], workerinput)
        directory = received.get("popcorn_cov_dir")
        if isinstance(directory, str):
            config.stash[COVERAGE] = CoverageRun(Path(directory), str(received["workerid"]), False)
        return
    if config.stash[SETTINGS].cov_enabled:
        directory = Path(tempfile.mkdtemp(prefix="popcorn-cov-"))
        config.stash[COVERAGE] = CoverageRun(directory, "main", True)
    if config.pluginmanager.hasplugin("xdist"):
        config.pluginmanager.register(XdistHooks(config), "popcorn-xdist")


@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(session: pytest.Session) -> None:
    """Merge every `.pcov` file of the session, write the reports, then remove the directory."""
    config = session.config
    run = config.stash.get(COVERAGE, None)
    settings = config.stash[SETTINGS]
    if run is None or not run.owner or not settings.cov_enabled:
        return
    failures = [f"popcorn: {failure}" for failure in _failures(config)]
    interrupted = session.exitstatus == pytest.ExitCode.INTERRUPTED
    totals = Totals()
    if interrupted:
        outcome = (
            "popcorn: session interrupted, no report written, the .pcov files stay in "
            f"{run.directory}"
        )
    elif not any(run.directory.glob("*.pcov")):
        shutil.rmtree(run.directory)
        outcome = "popcorn: no firmware coverage collected, no report written"
    else:
        try:
            merged = run.directory / "merged.info"
            merge_session(
                run.directory, merged, branches=True, unmodelled=settings.cov_unmodelled != "hit"
            )
            result = generate(
                merged,
                settings.cov_reports,
                include=settings.cov_include,
                omit=settings.cov_omit,
                root=config.rootpath,
                unmodelled=settings.cov_unmodelled,
            )
        except (OSError, ValueError) as error:
            outcome = (
                f"popcorn: coverage merge failed: {error}, the .pcov files stay in {run.directory}"
            )
        else:
            totals = result.totals
            written = [f"popcorn: firmware coverage written to {path}" for path in result.written]
            problems = [f"popcorn: {problem}" for problem in result.problems]
            if problems:
                problems.append(
                    f"popcorn: a report was not written, the .pcov files stay in {run.directory}"
                )
            else:
                shutil.rmtree(run.directory)
            outcome = "\n".join((*written, *problems))
            if result.term is not None:
                config.stash[TERM] = result.term
    threshold = settings.cov_fail_under
    if threshold is not None and not interrupted and below(totals, threshold):
        shown = format(Decimal(str(threshold)).normalize(), "f")
        failures.append(
            f"FAIL Required coverage of {shown}% not reached. "
            f"Total coverage: {truncated_percent(totals.lines)}%"
        )
        if session.exitstatus == pytest.ExitCode.OK:
            session.exitstatus = pytest.ExitCode.TESTS_FAILED
    config.stash[OUTCOME] = "\n".join(line for line in (outcome, *failures) if line)


def _failures(config: pytest.Config) -> list[str]:
    summaries = list(config.stash.get(WORKER_SUMMARIES, list[dict[str, object]]()))
    state = config.stash.get(SESSION, None)
    if state is not None:
        summaries.append(summary(state))
    return [
        str(failure)
        for one in summaries
        for failure in cast(list[object], one.get("coverage_failures", []))
    ]


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter) -> None:
    """The coverage outcome lines, then the terminal table under a `firmware coverage` header."""
    outcome = terminalreporter.config.stash.get(OUTCOME, None)
    if outcome is not None:
        for line in outcome.splitlines():
            terminalreporter.write_line(line)
    table = terminalreporter.config.stash.get(TERM, None)
    if table is not None:
        terminalreporter.write_sep("-", "firmware coverage")
        for line in table.splitlines():
            terminalreporter.write_line(line)
