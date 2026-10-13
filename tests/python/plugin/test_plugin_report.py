"""The "popcorn" report section of failing tests, FR-PYT-4, UC-7."""

from __future__ import annotations

import pytest

from .helpers import FAULT_ELF, write_conftest, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

HEADER = "*- popcorn -*"


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def test_an_assertion_after_a_run_reports_its_stop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import popcorn

        async def test_budget(machine):
            stop = await machine.run(budget=1000)
            assert stop.reason is popcorn.StopReason.ADDRESS
        """)
    result = run(pytester, "--popcorn-ring=50")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine (config=- digest=* strict=False):",
            "  stop: BUDGET at *Reset_Handler*",
            "  registers:",
            "  call stack:",
            "    #0 *Reset_Handler*",
            "  trace ring:",
        ]
    )
    result.stdout.no_fnmatch_line("*--popcorn-ring=N*")


FAULT = """
import popcorn

async def test_fault_in_a_handler(machine):
    machine.nvic.pend(0)
    stop = await machine.run(budget=1000)
    assert stop.reason is not popcorn.StopReason.FAULT
"""


def test_a_fault_reports_its_registers_and_frames(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile(FAULT)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [HEADER, "  stop: FAULT at *", "  fault: * CFSR 0x* HFSR 0x*", "    *: *", "    #1 *"]
    )


def test_a_failed_boot_reports_the_boot_stop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF, vtor="vt_nested")
    pytester.makepyfile("""
        def test_one(machine):
            pass

        def test_two(machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=2)
    lines = [
        HEADER,
        "  stop: FAULT at *level3*",
        "    UFSR: UNDEFINSTR",
        "    HFSR: FORCED",
        "    #0 *level3*",
        "    #1 *level2*",
        "    #2 *level1*",
        "    #3 *nested_main*",
    ]
    result.stdout.fnmatch_lines(lines + lines)


def test_an_exception_stop_is_reported(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile("""
        import popcorn

        async def test_background_fault(machine):
            machine.nvic.pend(0)
            async with machine.running():
                await machine.wait_for(popcorn.Reached("ready"))
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(["*FaultError*", HEADER, "  stop: FAULT at *"])


def test_the_ring_holds_this_test_only(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        async def test_first(machine):
            await machine.run(budget=100_000)

        async def test_second(machine):
            await machine.call("alu_mark")
            assert False, "second"
        """)
    result = run(pytester, "--popcorn-ring=4096")
    result.assert_outcomes(passed=1, failed=1)
    text = result.stdout.str()
    ring = text[text.index("trace ring:") : text.index("short test summary")]
    assert "RETURNED" in ring
    assert "BUDGET" not in ring


def test_no_section_for_passing_tests_or_tests_without_machines(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        def test_passes(machine):
            pass

        def test_fails_without_popcorn():
            assert False
        """)
    result = run(pytester, "-rP")
    result.assert_outcomes(passed=1, failed=1)
    result.stdout.no_fnmatch_line(HEADER)


def test_a_machine_without_a_run_shows_registers_and_ring(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        def test_no_run(machine):
            assert False
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(
        [HEADER, "machine (*):", "  no stop since the test started", "  registers:", "      r0 *"]
    )


def test_a_cancelled_run_leaves_no_stop_but_its_ring(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import asyncio
        import contextlib

        async def test_cancelled(machine):
            await machine.run(budget=100)
            with contextlib.suppress(TimeoutError):
                async with asyncio.timeout(0.005):
                    await machine.run()
            assert False
        """)
    result = run(pytester, "--popcorn-ring=50")
    result.stdout.fnmatch_lines(
        [HEADER, "  no stop since the test started", "  trace ring:", "* stop  BUDGET at *"]
    )


POLL = """
import popcorn

async def test_polls(machine):
    await machine.run(budget=20_000)
    assert False
"""


def test_the_ring_is_not_shown_by_default(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(POLL)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [HEADER, "  call stack:", "*", "  trace ring: not shown, pass --popcorn-ring=N *"]
    )
    result.stdout.no_fnmatch_line("  trace ring:")
    result.stdout.no_fnmatch_line("* block *")


def test_the_ring_is_shown_on_request(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(POLL)
    result = run(pytester, "--popcorn-ring=5")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [HEADER, "  call stack:", "*", "  trace ring:", "* stop  BUDGET at *"]
    )
    result.stdout.no_fnmatch_line("*--popcorn-ring=N*")


def test_a_negative_ring_is_a_usage_error(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = run(pytester, "--popcorn-ring=-1")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*--popcorn-ring must be 0 or above, not -1*"])


def test_an_error_stop_that_is_no_last_stop_is_printed_first(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import popcorn

        async def test_chained(machine):
            first = await machine.run(budget=100)
            await machine.run(budget=5000)
            try:
                raise popcorn.EmulationError("inner", stop=first)
            except popcorn.EmulationError as error:
                raise RuntimeError("outer") from error
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "exception stop: BUDGET at *",
            "machine (config=- digest=* strict=False):",
            "  stop: BUDGET at *",
            "    instructions 11???, *",
        ]
    )


def test_a_running_machine_is_parked_then_reported(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        KEEP = []

        async def test_left_running(machine):
            context = machine.running()
            KEEP.append(context)
            await context.__aenter__()
            assert False
        """)
    result = run(pytester, "-W", "ignore")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [HEADER, "machine (*):", "*no stop since the test started*", "*registers:*"]
    )
    result.stdout.no_fnmatch_line("*still running*")


def test_two_machines_get_two_blocks(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import popcorn

        async def test_two(machine, machine_factory, description):
            other = machine_factory(description)
            await other.run(budget=500)
            assert False
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine (config=- digest=* strict=False):",
            "  no stop since the test started",
            "machine_factory #1 (config=- digest=* strict=False):",
            "  stop: BUDGET at *",
        ]
    )


def test_the_machine_that_raised_is_marked(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile("""
        import popcorn

        async def test_two(machine, machine_factory, description):
            machine_factory(description)
            machine.nvic.pend(0)
            async with machine.running():
                await machine.wait_for(popcorn.Reached("ready"))
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine (config=- digest=* strict=False) (raised):",
            "  stop: FAULT at *",
            "machine_factory #1 (config=- digest=* strict=False):",
            "  no stop since the test started",
        ]
    )
    result.stdout.no_fnmatch_line("exception stop:*")


def test_a_broken_report_never_hides_the_failure(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        async def test_broken(machine, monkeypatch):
            def boom(*args, **kwargs):
                raise RuntimeError("report builder broke")

            monkeypatch.setattr("popcorn.pytest._report.stop_report", boom)
            await machine.run(budget=100)
            assert False, "the test's own failure"
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [
            "*AssertionError: the test's own failure*",
            HEADER,
            "popcorn report failed: RuntimeError: report builder broke",
        ]
    )


def test_the_section_crosses_xdist(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile(FAULT)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-p", "xdist", "-n", "2")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines([HEADER, "  stop: FAULT at *", "  fault: * CFSR 0x* HFSR 0x*"])


def test_the_faulting_factory_machine_carries_the_fault(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile("""
        import popcorn

        async def test_second_faults(machine_factory, description):
            first = await machine_factory.booted(description)
            await first.run(budget=100)
            second = machine_factory(description)
            second.nvic.pend(0)
            async with second.running():
                await second.wait_for(popcorn.Reached("ready"))
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine_factory #1 (config=- digest=* strict=False):",
            "  stop: BUDGET at *",
            "machine_factory #2 (config=- digest=* strict=False) (raised):",
            "  stop: FAULT at *",
            "  fault: * CFSR 0x* HFSR 0x*",
        ]
    )


def test_the_stop_is_found_inside_an_exception_group(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile("""
        import asyncio

        import popcorn

        async def test_task_group(machine):
            async def background():
                machine.nvic.pend(0)
                async with machine.running():
                    await machine.wait_for(popcorn.Reached("ready"))

            async with asyncio.TaskGroup() as group:
                group.create_task(background())
        """)
    result = run(pytester)
    result.stdout.fnmatch_lines(["*ExceptionGroup*", HEADER, "machine (*) (raised):"])


def test_an_xfailed_test_gets_no_section(pytester: pytest.Pytester) -> None:
    write_conftest(
        pytester,
        """
        from pathlib import Path

        def pytest_runtest_logreport(report):
            if report.when == "call":
                names = [name for name, _ in report.sections]
                Path("sections.txt").write_text(repr(names))
        """,
    )
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.xfail(reason="expected")
        def test_expected(machine):
            assert False
        """)
    result = run(pytester, "-rxX")
    result.assert_outcomes(xfailed=1)
    result.stdout.no_fnmatch_line(HEADER)
    assert "popcorn" not in (pytester.path / "sections.txt").read_text()


def test_machines_are_released_after_the_teardown_report(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import gc
        import weakref

        REFS = []

        def test_makes_a_machine(machine_factory, description):
            REFS.append(weakref.ref(machine_factory(description)))

        def test_it_is_gone():
            gc.collect()
            assert REFS[0]() is None
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_factory_boot_failure_is_reported(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF, vtor="vt_nested")
    pytester.makepyfile("""
        def test_factory(machine_factory, description):
            machine_factory(description)
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [
            HEADER,
            "machine_factory boot of config=- digest=* strict=False (*) (raised):",
            "  stop: FAULT at *",
        ]
    )


def test_a_teardown_error_gets_a_section(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.fixture
        def breaks_at_teardown():
            yield
            raise RuntimeError("teardown broke")

        def test_teardown(breaks_at_teardown, machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(["*RuntimeError: teardown broke*", HEADER, "machine (*):"])


def test_a_fault_inside_a_call_reports_the_faulting_frames(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="ready", elf=FAULT_ELF)
    pytester.makepyfile("""
        async def test_call_faults(machine):
            await machine.call("level1", 1, budget=10_000)
            assert False, "after the call"
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [HEADER, "  stop: FAULT at *level3*", "  call stack:", "    #0 *level3*", "    #1 *level2*"]
    )


GOAL_AFTER_PARK = """

import popcorn

_parked = popcorn.Machine._park
_posted = []


def _park_then_post(self):
    ended = _parked(self)
    if len(_posted) < POSTS:
        _posted.append(self._handle.run_goal(None, 10**12, None, None, None, None))
        assert self.busy
    return ended


popcorn.Machine._park = _park_then_post


def pytest_unconfigure(config):
    popcorn.Machine._park = _parked
"""


@pytest.mark.parametrize("posts", [1, 4], ids=["once", "every_park"])
def test_a_goal_posted_after_the_park_is_parked_again_before_the_reads(
    pytester: pytest.Pytester, posts: int
) -> None:
    """Posted after the report's park and after each of its 3 re-parks, the machine is
    reported as kept running."""
    write_conftest(pytester)
    write_project(pytester)
    conftest = pytester.path / "conftest.py"
    conftest.write_text(conftest.read_text() + f"\nPOSTS = {posts}\n" + GOAL_AFTER_PARK)
    pytester.makepyfile("""
        import popcorn

        async def test_budget(machine):
            stop = await machine.run(budget=1000)
            assert stop.reason is popcorn.StopReason.ADDRESS
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.no_fnmatch_line("*popcorn report failed*")
    if posts == 1:
        result.stdout.fnmatch_lines([HEADER, "machine (*):", "  stop: BUDGET at *"])
        result.stdout.no_fnmatch_line("*kept running*")
    else:
        result.stdout.fnmatch_lines(
            [HEADER, "machine (*):", "  machine kept running, state not read"]
        )


DELIVERED_END = """
import popcorn


class Never:
    def ready(self, machine):
        return False


async def test_reads_the_end_from_a_wait(machine):
    if KIND == "USER":
        machine.intercept("alu_mark", lambda call: call.stop())
    else:
        machine.nvic.pend(0)
    machine.start()
    stop = await machine.wait_for(Never())
    assert stop.reason is getattr(popcorn.StopReason, KIND)
"""


@pytest.mark.parametrize("kind", ["USER", "FAULT"])
def test_an_end_a_wait_returned_is_not_raised_again_at_teardown(
    pytester: pytest.Pytester, kind: str
) -> None:
    write_conftest(pytester)
    if kind == "FAULT":
        write_project(pytester, until="ready", elf=FAULT_ELF)
    else:
        write_project(pytester)
    pytester.makepyfile(f"KIND = {kind!r}\n" + DELIVERED_END)
    result = run(pytester)
    result.assert_outcomes(passed=1)
    result.stdout.no_fnmatch_line("*ERROR at teardown*")
