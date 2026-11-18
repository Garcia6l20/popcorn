"""`--popcorn-profile` and the `profile` marker keyword: one profile report per machine and
test."""

from __future__ import annotations

import contextlib
import re
import warnings
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pytest

from popcorn import Machine, PopcornError
from popcorn.pytest._options import SETTINGS
from popcorn.pytest._report import file_name
from popcorn.pytest._session import SESSION, WORKER_SUMMARIES, PopcornSession, summary

if TYPE_CHECKING:
    from popcorn.profiling import Profile


class ProfileRun:
    """The profile of one machine for one test, written whatever the test's outcome."""

    def __init__(
        self,
        item: pytest.Item,
        session: PopcornSession,
        label: str,
        analyses: tuple[str, ...],
    ) -> None:
        """Prepare the profile of the machine `label` of the test `item` with `analyses`, to
        `<profile_dir>/<node id>.<label>.json`."""
        directory = session.settings.profile_dir
        assert directory is not None
        name = f"{file_name(item, worker=False)}.{re.sub(r'[^A-Za-z0-9_.-]', '_', label)}"
        self.path = directory / f"{name}.json"
        self._item = item
        self._session = session
        self._label = label
        self._analyses = analyses
        self._stack = contextlib.ExitStack()
        self._profile: Profile | None = None

    def start(self, machine: Machine) -> bool:
        """Open the profile on the parked `machine`. A profile that cannot open is one
        PytestWarning, and the test runs unprofiled.

        Returns:
            Whether the profile opened.
        """
        try:
            self._profile = self._stack.enter_context(machine.profile(*self._analyses))
        except PopcornError as error:
            warnings.warn(
                pytest.PytestWarning(
                    f"popcorn could not profile {self._label} of {self._item.nodeid}: {error}"
                ),
                stacklevel=1,
            )
            return False
        return True

    def finish(self) -> None:
        """Close the profile of the parked machine, write its JSON report and add its text to
        the test's report sections. A report that cannot be written is one PytestWarning.

        Raises:
            RuntimeError: The profile could not be closed.
            Exception: What building the report, its text or its JSON raised.
            pytest.PytestWarning: The report could not be written, under
                `-W error::pytest.PytestWarning`.
        """
        try:
            self._stack.close()
        except Exception as error:
            raise RuntimeError(f"closing the popcorn profile of {self._label} failed") from error
        profile = self._profile
        if profile is None:
            return
        report = profile.report()
        part = self.path.with_suffix(".json.part")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            part.write_text(report.to_json(), encoding="utf-8")
            part.replace(self.path)
        except OSError as error:
            part.unlink(missing_ok=True)
            warnings.warn(
                pytest.PytestWarning(f"popcorn could not write {self.path}: {error}"),
                stacklevel=1,
            )
            return
        self._session.profiles += 1
        self._item.add_report_section(
            "teardown", f"popcorn profile {self._label}", f"{self.path}\n{report.text()}"
        )


def profiling(
    item: pytest.Item, session: PopcornSession, label: str, analyses: tuple[str, ...] | None
) -> ProfileRun | None:
    """A `ProfileRun` of the machine `label` for `item`, None when `analyses` is None."""
    if analyses is None:
        return None
    return ProfileRun(item, session, label, analyses)


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter) -> None:
    """One line naming the profiles written, summed over xdist workers on the controller."""
    config = terminalreporter.config
    summaries = list(config.stash.get(WORKER_SUMMARIES, list[dict[str, object]]()))
    state = config.stash.get(SESSION, None)
    if state is not None:
        summaries.append(summary(state))
    written = sum(cast(int, one.get("profiles", 0)) for one in summaries)
    if written:
        directory: Path | None = config.stash[SETTINGS].profile_dir
        terminalreporter.write_line(f"popcorn: {written} profile(s) written to {directory}")
