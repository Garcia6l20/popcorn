"""The "popcorn" section of a failing test's report: stop, fault, call stack and trace ring."""

from __future__ import annotations

from collections.abc import Generator
from dataclasses import dataclass
from typing import cast

import pytest

from popcorn import EmulationError, Machine, Stop
from popcorn.report import stop_report

_REGISTERS = (
    *(f"r{number}" for number in range(13)),
    "sp",
    "lr",
    "pc",
    "xpsr",
)


@dataclass(frozen=True, slots=True)
class Tracked:
    """A machine a test used.

    Attributes:
        label: How the report names it, `machine` or `machine_factory #<n>`.
        machine: The machine.
        key: Its config and strictness, as text.
    """

    label: str
    machine: Machine
    key: str


MACHINES = pytest.StashKey[list[Tracked]]()


def track(item: pytest.Item, label: str, machine: Machine, key: object) -> None:
    """Report `machine` under `label` if `item` fails."""
    item.stash.setdefault(MACHINES, list[Tracked]()).append(Tracked(label, machine, str(key)))


def exception_stop(error: BaseException | None) -> Stop | None:
    """The stop of the first error carrying one, depth first through exception groups, then
    through causes and contexts."""
    seen: set[int] = set()
    pending: list[BaseException | None] = [error]
    while pending:
        current = pending.pop()
        if current is None or id(current) in seen:
            continue
        seen.add(id(current))
        if isinstance(current, EmulationError) and current.stop is not None:
            return current.stop
        pending.append(current.__cause__ or current.__context__)
        if isinstance(current, BaseExceptionGroup):
            group = cast(BaseExceptionGroup[BaseException], current)
            pending.extend(reversed(group.exceptions))
    return None


def section_text(tracked: list[Tracked], error: BaseException | None) -> str:
    """The report of every tracked machine, in the order the test got them.

    A machine whose last stop is the stop the failing error carries is marked "(raised)". A stop
    of the error that is no machine's last stop is printed first.
    """
    stop = exception_stop(error)
    last = [None if entry.machine.busy else entry.machine.last_stop for entry in tracked]
    lines: list[str] = []
    if stop is not None and stop not in last:
        lines += [f"exception stop: {stop}", ""]
    for entry, own in zip(tracked, last, strict=True):
        raised = " (raised)" if stop is not None and own == stop else ""
        lines += [f"{entry.label} ({entry.key}){raised}:", _machine_text(entry.machine, own), ""]
    return "\n".join(lines).rstrip()


def _machine_text(machine: Machine, stop: Stop | None) -> str:
    if machine.busy:
        return "  still running: stop or await it before the test ends to get a report"
    if stop is not None:
        return _indent(stop_report(machine, stop).text())
    lines = ["no stop since the test started", "registers:"]
    values = {name: int(getattr(machine.reg, name)) for name in _REGISTERS}
    names = list(values)
    for row in range(0, len(names), 4):
        cells = [f"{name:>4} {values[name]:#010x}" for name in names[row : row + 4]]
        lines.append("  " + "  ".join(cells))
    ring = machine.ring.dump()
    if ring:
        lines.append("trace ring:")
        lines.extend(f"  {line}" for line in ring.splitlines())
    return _indent("\n".join(lines))


def _indent(text: str) -> str:
    return "\n".join(f"  {line}" for line in text.splitlines())


@pytest.hookimpl(wrapper=True, tryfirst=True)
def pytest_runtest_makereport(
    item: pytest.Item, call: pytest.CallInfo[None]
) -> Generator[None, pytest.TestReport, pytest.TestReport]:
    """Add the "popcorn" section to the report of a failing phase of a test that used a machine,
    and release the test's machines after its teardown report.

    `tryfirst` makes this wrapper the outermost, so it sees the outcome after xfail handling.
    """
    report = yield
    tracked = item.stash.get(MACHINES, list[Tracked]())
    if report.failed and tracked:
        error = None if call.excinfo is None else call.excinfo.value
        try:
            text = section_text(tracked, error)
        except Exception as broken:
            text = f"popcorn report failed: {type(broken).__name__}: {broken}"
        report.sections.append(("popcorn", text))
    if call.when == "teardown" and MACHINES in item.stash:
        del item.stash[MACHINES]
    return report
