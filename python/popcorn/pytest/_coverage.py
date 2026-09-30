"""`--popcorn-cov`: one `.pcov` file per process, merged into one lcov file at session end."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path
from typing import cast

import pytest

from popcorn.coverage import merge_session
from popcorn.pytest._options import SETTINGS
from popcorn.pytest._session import COVERAGE, SESSION, WORKER_SUMMARIES, CoverageRun, summary

OUTCOME = pytest.StashKey[str]()


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
    if config.stash[SETTINGS].cov is not None:
        directory = Path(tempfile.mkdtemp(prefix="popcorn-cov-"))
        config.stash[COVERAGE] = CoverageRun(directory, "main", True)
    if config.pluginmanager.hasplugin("xdist"):
        config.pluginmanager.register(XdistHooks(config), "popcorn-xdist")


@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(session: pytest.Session) -> None:
    """Merge every `.pcov` file of the session into the lcov file, then remove the directory."""
    config = session.config
    run = config.stash.get(COVERAGE, None)
    lcov = config.stash[SETTINGS].cov
    if run is None or not run.owner or lcov is None:
        return
    failures = _failures(config)
    if session.exitstatus == pytest.ExitCode.INTERRUPTED:
        outcome = (
            "popcorn: session interrupted, no lcov written, the .pcov files stay in "
            f"{run.directory}"
        )
    elif not any(run.directory.glob("*.pcov")):
        shutil.rmtree(run.directory)
        outcome = "popcorn: no firmware coverage collected, no lcov written"
    else:
        try:
            merge_session(run.directory, lcov, branches=True)
        except (OSError, ValueError) as error:
            outcome = (
                f"popcorn: coverage merge failed: {error}, the .pcov files stay in {run.directory}"
            )
        else:
            shutil.rmtree(run.directory)
            outcome = f"popcorn: firmware coverage written to {lcov}"
    config.stash[OUTCOME] = "\n".join([outcome, *(f"popcorn: {failure}" for failure in failures)])


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
    """The coverage outcome line."""
    outcome = terminalreporter.config.stash.get(OUTCOME, None)
    if outcome is not None:
        for line in outcome.splitlines():
            terminalreporter.write_line(line)
