"""Machines left started by a test: parked at teardown, their free run's end raised there, the
report reading them parked, the session ending without a hang."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import ROOT, write_conftest, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")
HEADER = "*- popcorn -*"
EXIT_WITHIN = 30

DRIVER_C = ROOT / "tests" / "firmware" / "api" / "driver.c"

RAISING_WATCH = """
import threading

class Boom(Exception):
    pass

def raising(machine):
    fired = threading.Event()

    def boom(hit):
        fired.set()
        raise Boom("from the watch")

    machine.watch("alu_value", handler=boom)
    return fired


def marked(machine):
    fired = threading.Event()
    machine.watch("alu_rounds", handler=lambda hit: fired.set())
    return fired
"""

LEFT_RUNNING = "*popcorn: * was left running and its free run ended with Boom(*"


@pytest.fixture(autouse=True)
def _project(pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch) -> None:
    # The count asserts below also match pytest's short summary line, which shows
    # the exception message only when the terminal is wide enough. Pin the width.
    monkeypatch.setenv("COLUMNS", "80")
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(raising_watch=RAISING_WATCH)


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def test_a_started_machine_is_parked_and_reused(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        SEEN = []

        def test_starts(machine):
            SEEN.append(machine)
            machine.start()
            assert machine.busy

        def test_reuses(machine):
            assert machine is SEEN[0]
            assert not machine.busy
            assert machine.pc == machine.sym("alu_mark")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_a_handler_error_of_a_free_run_errors_at_teardown_once(
    pytester: pytest.Pytester,
) -> None:
    pytester.makepyfile("""
        from raising_watch import raising

        def test_body_passes(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)

        def test_next(machine):
            assert not machine.busy

        def test_factory_body_passes(machine_factory, description):
            other = machine_factory(description)
            fired = raising(other)
            other.start()
            assert fired.wait(10)
        """)
    result = run(pytester)
    result.assert_outcomes(passed=3, errors=2)
    result.stdout.fnmatch_lines(
        [
            "*ERROR at teardown of test_body_passes*",
            "*ERROR at teardown of test_factory_body_passes*",
        ]
    )
    assert result.stdout.str().count("Boom: from the watch") == 2


def test_a_body_raising_the_free_run_error_shows_it_once(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        import popcorn
        from raising_watch import raising

        async def test_waits(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            await machine.wait_for(popcorn.Reached("alu_mark"))
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    assert result.stdout.str().count("Boom: from the watch") == 1


def test_an_end_the_body_caught_is_not_raised_again(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        import popcorn
        import pytest
        from raising_watch import Boom, raising

        async def test_expects_it(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            with pytest.raises(Boom):
                await machine.wait_for(popcorn.Reached("alu_mark"))

        async def test_wraps_it(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            try:
                await machine.wait_for(popcorn.Reached("alu_mark"))
            except Boom as boom:
                raise RuntimeError("wrapped") from boom

        @pytest.mark.xfail(raises=Boom, strict=True)
        async def test_expected_to_fail(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            await machine.wait_for(popcorn.Reached("alu_mark"))
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1, xfailed=1)
    result.stdout.no_fnmatch_line("*ERROR at teardown*")


def test_a_failing_test_reports_its_running_machine_parked(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        from raising_watch import raising

        async def test_fails_running(machine):
            await machine.run(budget=100)
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            assert False, "the body"
        """)
    result = run(pytester, "--popcorn-ring=4")
    result.assert_outcomes(failed=1, errors=1)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine (*):",
            "*no last stop: the machine stands where its last run, if any, left it*",
            "*registers:*",
            "*trace ring:*",
            "* stop  BUDGET at *",
        ]
    )
    result.stdout.no_fnmatch_line("*still running*")
    result.stdout.no_fnmatch_line("*popcorn report failed*")
    assert result.stdout.str().count("Boom: from the watch") == 1


def test_factory_ends_after_the_first_are_warned(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        from raising_watch import raising

        def test_two_end(machine_factory, description):
            for _ in range(2):
                other = machine_factory(description)
                fired = raising(other)
                other.start()
                assert fired.wait(10)
        """)
    result = run(pytester, "-W", "error")
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(["*popcorn: machine_factory #2 of * was left running*Boom(*"])


def test_an_end_no_fixture_claims_is_warned_even_under_w_error(
    pytester: pytest.Pytester,
) -> None:
    pytester.makepyfile("""
        import pytest
        from raising_watch import raising

        KEPT = []

        @pytest.fixture(scope="session", autouse=True)
        def restarts_then_fails():
            yield
            fired = raising(KEPT[0])
            KEPT[0].start()
            assert fired.wait(10)
            raise RuntimeError("fixture teardown")

        def test_restarted(machine):
            KEPT.append(machine)
        """)
    result = run(pytester, "-W", "error")
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.no_fnmatch_line("*INTERNALERROR*")
    result.stdout.fnmatch_lines([LEFT_RUNNING])


def test_an_end_at_session_end_is_warned_even_under_w_error(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        import pytest
        from raising_watch import raising

        KEPT = []

        @pytest.fixture(scope="session", autouse=True)
        def restarts_at_session_end():
            yield
            fired = raising(KEPT[0])
            KEPT[0].start()
            assert fired.wait(10)

        def test_keeps(machine):
            KEPT.append(machine)
        """)
    result = run(pytester, "-W", "error")
    result.assert_outcomes(passed=1)
    result.stdout.no_fnmatch_line("*INTERNALERROR*")
    result.stdout.fnmatch_lines([LEFT_RUNNING])


def test_factory_machines_left_started_are_parked_and_counted(
    pytester: pytest.Pytester,
) -> None:
    pytester.makepyfile("""
        def test_two_started(machine_factory, description):
            for _ in range(2):
                machine_factory(description).start()
        """)
    result = run(pytester, "--popcorn-cov=cov.info")
    result.assert_outcomes(passed=1)
    result.stdout.no_fnmatch_line("*MachineRunning*")
    result.stdout.fnmatch_lines(["popcorn: firmware coverage written to *cov.info"])


@pytest.mark.timeout(40)
def test_a_session_ends_with_a_started_machine_in_a_global(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        KEPT = []

        def test_keeps_one_running(description):
            machine = description.machine()
            machine.reset()
            machine.start()
            KEPT.append(machine)
        """)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, timeout=EXIT_WITHIN)
    result.assert_outcomes(passed=1)
    assert result.ret == 0


@pytest.mark.timeout(40)
def test_xdist_workers_park_and_exit(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    pytester.makepyfile("""
        from raising_watch import marked

        def test_one(machine):
            fired = marked(machine)
            machine.start()
            assert fired.wait(10)

        def test_two(machine):
            fired = marked(machine)
            machine.start()
            assert fired.wait(10)
        """)
    result = pytester.runpytest_subprocess(
        *NO_ASYNCIO, "-n", "2", "--popcorn-cov=cov.info", timeout=EXIT_WITHIN
    )
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["popcorn: firmware coverage written to *cov.info"])
    line = _line_of(DRIVER_C, "alu_rounds + 1u")
    assert _hits(pytester.path / "cov.info", DRIVER_C, line) > 0


def _line_of(source: Path, text: str) -> int:
    lines = source.read_text().splitlines()
    return next(number for number, line in enumerate(lines, start=1) if text in line)


def _hits(lcov: Path, source: Path, line: int) -> int:
    current = ""
    for record in lcov.read_text().splitlines():
        if record.startswith("SF:"):
            current = record[3:]
        elif current.endswith(source.name) and record.startswith(f"DA:{line},"):
            return int(record.split(",")[1])
    return 0


@pytest.mark.parametrize("end", ["error", "stop"])
def test_a_fixture_halt_after_a_failed_test_raises_the_end_the_report_parked(
    pytester: pytest.Pytester, end: str
) -> None:
    pytester.makepyfile(f"""
        import threading

        import popcorn
        import pytest
        from raising_watch import Boom, raising

        END = {end!r}


        def stopping(machine):
            fired = threading.Event()

            def stop(hit):
                fired.set()
                return popcorn.WatchAction.STOP

            machine.watch("alu_value", handler=stop)
            return fired


        @pytest.fixture
        def started(machine):
            fired = raising(machine) if END == "error" else stopping(machine)
            machine.start()
            assert fired.wait(10)
            yield machine
            expected = Boom if END == "error" else popcorn.EmulationError
            with pytest.raises(expected):
                machine.halt()
            machine.halt()


        def test_fails(started):
            assert False, "the body"
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.no_fnmatch_line("*ERROR at teardown*")
    result.stdout.no_fnmatch_line("*was left running*")


TWO_ENDS = """

import time

import popcorn

_parked = popcorn.Machine._park
_started = []


def _park_then_end_again(self):
    ended = _parked(self)
    if not _started:
        _started.append(True)
        self.start()
        time.sleep(0.5)
    return ended


popcorn.Machine._park = _park_then_end_again


def pytest_unconfigure(config):
    popcorn.Machine._park = _parked
"""


def test_an_end_a_report_repark_takes_is_kept_beside_the_first(
    pytester: pytest.Pytester,
) -> None:
    """The report's park takes the handler error, a free run started after it ends by itself
    on the same raising watch before the read, half a second of wall time, and the re-park
    takes that second error: the teardown raises the first and warns the second."""
    conftest = pytester.path / "conftest.py"
    conftest.write_text(conftest.read_text() + TWO_ENDS)
    pytester.makepyfile("""
        from raising_watch import raising

        def test_fails(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
            assert False, "the body"
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1, errors=1)
    result.stdout.fnmatch_lines(["*ERROR at teardown of test_fails*", LEFT_RUNNING])


FINISH_RAISES = """

from popcorn.pytest import _report


_finish = _report.TraceOnFail.finish


def _refuse(self):
    raise OSError("the trace could not be written")


_report.TraceOnFail.finish = _refuse


def pytest_unconfigure(config):
    _report.TraceOnFail.finish = _finish
"""


def test_a_held_end_is_warned_when_the_trace_finish_raises(pytester: pytest.Pytester) -> None:
    conftest = pytester.path / "conftest.py"
    conftest.write_text(conftest.read_text() + FINISH_RAISES)
    pytester.makepyfile("""
        from raising_watch import raising

        def test_left_running(machine):
            fired = raising(machine)
            machine.start()
            assert fired.wait(10)
        """)
    result = run(pytester, "--popcorn-trace-on-fail")
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(
        [
            "*ERROR at teardown of test_left_running*",
            "*OSError: the trace could not be written*",
            LEFT_RUNNING,
        ]
    )


KEPT_LOG_ERROR = """

import logging


class FailOnce(logging.Filter):
    def __init__(self):
        super().__init__()
        self.failed = False

    def filter(self, record):
        if not self.failed:
            self.failed = True
            raise RuntimeError("filter failed")
        return True


_failing = FailOnce()
logging.getLogger("popcorn").addFilter(_failing)


def pytest_unconfigure(config):
    logging.getLogger("popcorn").removeFilter(_failing)
"""


def test_a_log_error_kept_by_an_await_does_not_break_the_report_or_the_teardown(
    pytester: pytest.Pytester,
) -> None:
    conftest = pytester.path / "conftest.py"
    conftest.write_text(conftest.read_text() + KEPT_LOG_ERROR)
    pytester.makepyfile("""
        import struct

        def load_a_hole(machine):
            found = next(p for p in machine.description.peripherals if p.name == "TESTDEV")
            machine.mem.write(0x2008_0000, struct.pack("<2H", 0x7803, 0xBE01))
            machine.reg.r0 = found.base + 0x100
            machine.reg.pc = 0x2008_0000

        async def test_fails_after_a_kept_error(machine):
            machine.intercept("alu_mark", lambda call: call.proceed())
            load_a_hole(machine)
            await machine.run(budget=2)
            assert False, "the body"

        def test_next_is_clean(machine):
            assert machine.intercept_log("alu_mark") == []
            assert not machine.busy
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1, errors=1)
    result.stdout.no_fnmatch_line("*INTERNALERROR*")
    result.stdout.fnmatch_lines(["*ERROR at teardown of test_fails_after_a_kept_error*"])
    assert result.stdout.str().count("RuntimeError: filter failed") == 1
