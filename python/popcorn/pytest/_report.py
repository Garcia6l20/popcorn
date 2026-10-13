"""The "popcorn" section of a failing test's report: stop, fault, call stack and trace ring."""

from __future__ import annotations

import contextlib
import hashlib
import re
import warnings
from collections.abc import Generator
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest

from popcorn import EmulationError, Machine, MachineRunning, Stop, System
from popcorn._obs import FormatError
from popcorn.pytest._options import SETTINGS
from popcorn.report import stop_report
from popcorn.trace import ALL

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
        label: How the report names it: the declared machine fixture's name,
            `machine_factory #<n>` or `<system fixture> member <name>`.
        machine: The machine.
        key: Its machine description, config and strictness, as text.
    """

    label: str
    machine: Machine
    key: str


MACHINES = pytest.StashKey[list[Tracked]]()
NAME_LENGTH = 150
RING_HINT = "trace ring: not shown, pass --popcorn-ring=N for its last N lines"
CALL_FAILED = pytest.StashKey[bool]()
KEPT_TRACES = pytest.StashKey[list[Path]]()
HELD = pytest.StashKey[dict[int, BaseException | None]]()
SYSTEMS = pytest.StashKey[list[System]]()


class TraceOnFail:
    """A full trace of one test's machine, kept only when the test's call fails."""

    def __init__(self, item: pytest.Item, directory: Path, member: str) -> None:
        """Prepare the trace of the test `item` in `directory`, `<name>.<member>.pctr`, with
        `member` the declared machine fixture's name or `<system fixture>.<member>`."""
        name = f"{file_name(item)}.{re.sub(r'[^A-Za-z0-9_.-]', '_', member)}"
        self.path = directory / f"{name}.pctr"
        self._directory = directory
        self._part = directory / f"{name}.pctr.part"
        self._item = item
        self._stack = contextlib.ExitStack()

    def start(self, machine: Machine) -> bool:
        """Start tracing `machine`. A trace that cannot start is one PytestWarning, and the test
        runs untraced.

        Returns:
            Whether the trace started.
        """
        try:
            self._directory.mkdir(parents=True, exist_ok=True)
            self._stack.enter_context(machine.trace(ALL, self._part))
        except (OSError, FormatError) as error:
            warnings.warn(
                pytest.PytestWarning(f"popcorn could not trace {self._item.nodeid}: {error}"),
                stacklevel=1,
            )
            return False
        self._item.stash.setdefault(KEPT_TRACES, list[Path]()).append(self.path)
        return True

    def finish(self) -> None:
        """Close the trace, then keep it when the test's call failed, else delete it.

        Raises:
            RuntimeError: The trace could not be closed, naming its file.
        """
        kept = False
        try:
            try:
                self._stack.close()
            except Exception as error:
                raise RuntimeError(f"closing the popcorn trace {self._part} failed") from error
            if self._item.stash.get(CALL_FAILED, False):
                self._part.replace(self.path)
                kept = True
        finally:
            if not kept:
                self._part.unlink(missing_ok=True)


def sweep(directory: Path) -> None:
    """Delete the part files an interrupted session left in `directory`."""
    if directory.is_dir():
        for part in directory.glob("*.pctr.part"):
            part.unlink(missing_ok=True)


def trace_directory(config: pytest.Config, chosen: Path | None) -> Path:
    """Where kept traces go: `chosen`, else the pytest cache, else `<rootdir>/.popcorn-traces`."""
    if chosen is not None:
        return chosen
    cache: object = getattr(config, "cache", None)
    if isinstance(cache, pytest.Cache):
        return cache.mkdir("popcorn") / "traces"
    return config.rootpath / ".popcorn-traces"


def file_name(item: pytest.Item) -> str:
    """A file name for the test `item`: its node id with other characters than letters, digits,
    `_`, `.` and `-` replaced, cut to `NAME_LENGTH`, then a hash of the node id when that changed
    it, and the xdist worker id first."""
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", item.nodeid)
    if name != item.nodeid or len(name) > NAME_LENGTH:
        digest = hashlib.sha256(item.nodeid.encode()).hexdigest()[:8]
        name = f"{name[:NAME_LENGTH]}-{digest}"
    workerinput: object = getattr(item.config, "workerinput", None)
    if isinstance(workerinput, dict):
        worker = cast(dict[str, object], workerinput).get("workerid")
        if isinstance(worker, str):
            name = f"{worker}-{name}"
    return name


def track(item: pytest.Item, label: str, machine: Machine, key: object) -> None:
    """Report `machine` under `label` if `item` fails."""
    item.stash.setdefault(MACHINES, list[Tracked]()).append(Tracked(label, machine, str(key)))


def track_system(item: pytest.Item, system: System) -> None:
    """Close `system` before a failure report of `item` parks its members."""
    item.stash.setdefault(SYSTEMS, list[System]()).append(system)


def park(item: pytest.Item) -> None:
    """Park every machine `item` tracks and keep what ended each free run by itself, and never
    reached the test, for the teardown of its fixture. The linked systems of `item` are closed
    first: a member of a running system cannot park. What a system kept for its teardown stays
    kept."""
    for system in item.stash.get(SYSTEMS, list[System]()):
        system.close()
    held = item.stash.setdefault(HELD, dict[int, BaseException | None]())
    for entry in item.stash.get(MACHINES, list[Tracked]()):
        if id(entry.machine) not in held:
            held[id(entry.machine)] = park_ended(entry.machine)


def park_ended(machine: Machine) -> BaseException | None:
    """Park `machine`, returning what ended its free run by itself instead of raising it, None
    when a wait already returned or raised it to the test."""
    return machine._park()  # pyright: ignore[reportPrivateUsage]


def kept_ends(item: pytest.Item, machine: Machine) -> list[BaseException]:
    """Park `machine` and take every end its parks for `item` kept and no user's `halt()`
    raised, oldest first, the report's park first."""
    take_held(item, machine)
    park_ended(machine)
    return machine._take_parked_ends()  # pyright: ignore[reportPrivateUsage]


def take_held(item: pytest.Item, machine: Machine) -> BaseException | None:
    """What ended the free run of `machine` when the report of `item` parked it, removed."""
    return item.stash.get(HELD, dict[int, BaseException | None]()).pop(id(machine), None)


def unclaimed(owner: str, ended: BaseException) -> None:
    """Warn that `ended` ended a free run of `owner` and no test can raise it any more.

    The warning is shown even under `-W error`, never raised: it is emitted from report hooks,
    session end and cleanup that must finish.
    """
    with warnings.catch_warnings():
        warnings.simplefilter("always", pytest.PytestUnraisableExceptionWarning)
        warnings.warn(
            pytest.PytestUnraisableExceptionWarning(
                f"popcorn: {owner} was left running and its free run ended with {ended!r}"
            ),
            stacklevel=1,
        )


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


KEPT_RUNNING = "  machine kept running, state not read"
PARK_TRIES = 3


def section_text(
    tracked: list[Tracked], error: BaseException | None, ring: int, item: pytest.Item
) -> str:
    """The report of every tracked machine, in the order the test got them, each with the last
    `ring` lines of its collapsed trace ring, or a hint naming `--popcorn-ring` for 0.

    A machine whose last stop is the stop the failing error carries is marked "(raised)". A stop
    of the error that is no machine's last stop is printed first. Each machine is read in one
    parked section. A machine found running again, a goal another thread started after the
    park, is parked again up to `PARK_TRIES` times, an end that park returns kept for `item`'s
    teardown, then reported as kept running.
    """
    stop = exception_stop(error)
    reads = [_parked_read(item, entry.machine, ring) for entry in tracked]
    last = [read[0] for read in reads if read is not None]
    lines: list[str] = []
    if stop is not None and stop not in last:
        lines += [f"exception stop: {stop}", ""]
    for entry, read in zip(tracked, reads, strict=True):
        if read is None:
            lines += [f"{entry.label} ({entry.key}):", KEPT_RUNNING, ""]
            continue
        own, text = read
        raised = " (raised)" if stop is not None and own == stop else ""
        lines += [f"{entry.label} ({entry.key}){raised}:", text, ""]
    return "\n".join(lines).rstrip()


def _parked_read(item: pytest.Item, machine: Machine, ring: int) -> tuple[Stop | None, str] | None:
    entered = False

    def read() -> tuple[Stop | None, str]:
        nonlocal entered
        entered = True
        own = machine.last_stop
        return own, _machine_text(machine, own, ring)

    for attempt in range(PARK_TRIES + 1):
        try:
            return cast(
                tuple[Stop | None, str],
                machine._handle.parked(read),  # pyright: ignore[reportPrivateUsage]
            )
        except MachineRunning:
            if entered:
                raise
        if attempt == PARK_TRIES:
            break
        ended = park_ended(machine)
        if ended is not None:
            held = item.stash.setdefault(HELD, dict[int, BaseException | None]())
            if held.get(id(machine)) is None:
                held[id(machine)] = ended
    return None


def _machine_text(machine: Machine, stop: Stop | None, ring: int) -> str:
    if stop is not None:
        report = stop_report(machine, stop, ring=ring > 0, ring_last=ring or None, collapse=True)
        lines = [report.text()]
    else:
        lines = ["no stop since the test started", "registers:"]
        values = {name: int(getattr(machine.reg, name)) for name in _REGISTERS}
        names = list(values)
        for row in range(0, len(names), 4):
            cells = [f"{name:>4} {values[name]:#010x}" for name in names[row : row + 4]]
            lines.append("  " + "  ".join(cells))
        dump = machine.ring.dump(last=ring, collapse=True) if ring else ""
        if dump:
            lines.append("trace ring:")
            lines.extend(f"  {line}" for line in dump.splitlines())
    if not ring:
        lines.append(RING_HINT)
    return _indent("\n".join(lines))


def _indent(text: str) -> str:
    return "\n".join(f"  {line}" for line in text.splitlines())


@pytest.hookimpl(tryfirst=True)
def pytest_sessionstart(session: pytest.Session) -> None:
    """Sweep stale trace part files, from the xdist controller or a run without xdist."""
    settings = session.config.stash[SETTINGS]
    if settings.trace_on_fail and getattr(session.config, "workerinput", None) is None:
        sweep(trace_directory(session.config, settings.trace_dir))


@pytest.hookimpl(wrapper=True, tryfirst=True)
def pytest_runtest_makereport(
    item: pytest.Item, call: pytest.CallInfo[None]
) -> Generator[None, pytest.TestReport, pytest.TestReport]:
    """Add the "popcorn" section to the report of a failing phase of a test that used a machine,
    parking its machines first, and release the test's machines after its teardown report.

    `tryfirst` makes this wrapper the outermost, so it sees the outcome after xfail handling.
    """
    report = yield
    tracked = item.stash.get(MACHINES, list[Tracked]())
    if call.when == "call" and report.failed:
        item.stash[CALL_FAILED] = True
    error = None if call.excinfo is None else call.excinfo.value
    if tracked and (report.failed or error is not None):
        park(item)
    if report.failed and tracked:
        try:
            text = section_text(tracked, error, item.config.stash[SETTINGS].ring, item)
        except Exception as broken:
            text = f"popcorn report failed: {type(broken).__name__}: {broken}"
        if call.when == "call":
            for kept in item.stash.get(KEPT_TRACES, list[Path]()):
                text += f"\ntrace: {kept}"
        report.sections.append(("popcorn", text))
    if call.when == "teardown":
        if MACHINES in item.stash:
            del item.stash[MACHINES]
        if SYSTEMS in item.stash:
            del item.stash[SYSTEMS]
        if HELD in item.stash:
            for ended in item.stash[HELD].values():
                if ended is not None:
                    unclaimed(item.nodeid, ended)
            del item.stash[HELD]
    return report
