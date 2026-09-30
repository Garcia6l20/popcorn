"""Async tests on the plugin's own event loop, without pytest-asyncio, NFR-ASYNC-1."""

from __future__ import annotations

import json
import subprocess
import sys

import pytest

from .helpers import DRIVER_ELF, M33_JSON, write_conftest

pytest_plugins = ["pytester"]

RECORD_LOOPS = """
import asyncio
import json
from pathlib import Path

import pytest

LOOPS = []


def record(loop):
    LOOPS.append(loop)


@pytest.hookimpl(trylast=True)
def pytest_unconfigure(config):
    Path("loops.json").write_text(
        json.dumps({"ids": [id(loop) for loop in LOOPS], "closed": [l.is_closed() for l in LOOPS]})
    )
"""


def test_an_async_test_runs_without_pytest_asyncio(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import asyncio

        async def test_sleeps():
            await asyncio.sleep(0)
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=1)


def test_without_the_plugin_an_async_test_fails(pytester: pytest.Pytester) -> None:
    pytester.makepyfile("""
        async def test_sleeps():
            pass
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(failed=1)


def test_a_failing_async_assert_fails_with_its_message(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import asyncio

        async def test_fails():
            await asyncio.sleep(0)
            assert 1 == 2, "the async message"
        """)
    result = pytester.runpytest("-p", "no:asyncio")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(["*AssertionError: the async message*"])


def test_async_tests_share_one_loop_closed_after_the_session(pytester: pytest.Pytester) -> None:
    write_conftest(pytester, RECORD_LOOPS)
    pytester.makepyfile("""
        import asyncio

        from conftest import record

        async def test_one():
            record(asyncio.get_running_loop())

        async def test_two():
            record(asyncio.get_running_loop())
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=2)
    loops = json.loads((pytester.path / "loops.json").read_text())
    assert len(loops["ids"]) == 2
    assert loops["ids"][0] == loops["ids"][1]
    assert loops["closed"] == [True, True]


def test_fixtures_parametrize_and_methods_reach_the_test(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import asyncio
        import os
        from unittest import mock

        import pytest

        @pytest.fixture
        def seven():
            return 7

        @pytest.fixture(autouse=True)
        def unused():
            return "not an argument"

        @pytest.mark.parametrize("n", [1, 2])
        async def test_parametrized(seven, n):
            await asyncio.sleep(0)
            assert seven + n in (8, 9)

        async def test_default_is_not_a_fixture(seven, other=3):
            assert (seven, other) == (7, 3)

        class TestInClass:
            async def test_method(self, seven):
                assert seven == 7

        @mock.patch("os.getcwd", return_value="patched")
        async def test_mock_patch(patched, seven):
            assert (os.getcwd(), patched.call_count, seven) == ("patched", 1, 7)
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=5)


def test_an_anyio_test_runs_on_anyio_loop(pytester: pytest.Pytester) -> None:
    pytest.importorskip("anyio")
    write_conftest(pytester, RECORD_LOOPS)
    pytester.makepyfile("""
        import asyncio

        import pytest

        from conftest import record

        async def test_owned():
            record(asyncio.get_running_loop())

        @pytest.mark.anyio
        async def test_anyio(anyio_backend):
            record(asyncio.get_running_loop())

        @pytest.fixture
        def anyio_backend():
            return "asyncio"
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=2)
    loops = json.loads((pytester.path / "loops.json").read_text())
    assert loops["ids"][0] != loops["ids"][1]


def test_leftover_tasks_are_cancelled_with_a_warning(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import asyncio

        STATE = {}

        async def test_leaves_a_task():
            async def sleeper():
                try:
                    await asyncio.sleep(10)
                except asyncio.CancelledError:
                    STATE["cancelled"] = True
                    raise

            STATE["task"] = asyncio.create_task(sleeper(), name="sleeper")
            await asyncio.sleep(0)

        async def test_next():
            assert STATE["cancelled"]
            assert STATE["task"].cancelled()
        """)
    result = pytester.runpytest("-p", "no:asyncio")
    result.assert_outcomes(passed=2, warnings=1)
    result.stdout.fnmatch_lines(["*popcorn cancelled 1 task(s) the test left running: sleeper*"])


def test_a_task_ignoring_cancellation_is_named_and_never_blocks(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(
        pytester,
        """
        def pytest_configure(config):
            import popcorn.pytest._loop

            popcorn.pytest._loop.CANCEL_WAIT = 0.2
        """,
    )
    pytester.makepyfile("""
        import asyncio

        async def stubborn():
            while True:
                try:
                    await asyncio.sleep(0.01)
                except asyncio.CancelledError:
                    pass

        async def failing():
            try:
                await asyncio.sleep(10)
            except asyncio.CancelledError:
                raise ValueError("cleanup broke")

        async def test_leaves_tasks():
            asyncio.create_task(stubborn(), name="stubborn")
            asyncio.create_task(failing(), name="failing")
            await asyncio.sleep(0)

        async def test_next():
            await asyncio.sleep(0)
        """)
    result = pytester.runpytest("-p", "no:asyncio")
    result.assert_outcomes(passed=2, warnings=1)
    result.stdout.fnmatch_lines(
        [
            "*popcorn cancelled 2 task(s) the test left running: failing, stubborn;"
            " still running after 0.2 s: stubborn; errors: ValueError('cleanup broke')*"
        ]
    )


def test_a_sync_asyncio_run_between_async_tests(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import asyncio

        LOOPS = []

        async def test_first():
            LOOPS.append(asyncio.get_running_loop())

        def test_sync_run():
            async def inner():
                return asyncio.get_running_loop()

            assert asyncio.run(inner()) is not LOOPS[0]

        async def test_last():
            assert asyncio.get_running_loop() is LOOPS[0]
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=3)


def test_an_async_test_drives_a_machine_without_blocking_the_loop(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    pytester.makepyfile(f"""
        import asyncio

        import popcorn

        def machine():
            m = popcorn.Machine(
                popcorn.load_description({str(M33_JSON)!r}),
                image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
            )
            m.reset()
            return m

        async def test_until_alu_mark():
            m = machine()
            stop = await m.run(until="alu_mark")
            assert stop.reason is popcorn.StopReason.ADDRESS

        async def test_a_ticker_runs_during_the_run():
            m = machine()
            ticks = 0
            done = False

            async def ticker():
                nonlocal ticks
                while not done:
                    ticks += 1
                    await asyncio.sleep(0)

            task = asyncio.create_task(ticker())
            stop = await m.run(budget=50_000_000)
            done = True
            await task
            assert stop.reason is popcorn.StopReason.BUDGET
            assert ticks >= 10

        async def test_a_wall_timeout_cancels_and_the_machine_resumes():
            m = machine()
            await m.run(until="alu_mark")
            try:
                async with asyncio.timeout(0.01):
                    await m.run()
            except TimeoutError:
                pass
            else:
                raise AssertionError("the endless run ended")
            stop = await m.run(budget=1000)
            assert stop.reason is popcorn.StopReason.BUDGET
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=3)


def test_the_plugin_imports_without_pytest_asyncio_or_xdist() -> None:
    code = (
        "import sys\n"
        "sys.modules['pytest_asyncio'] = None\n"
        "sys.modules['xdist'] = None\n"
        "import popcorn.pytest\n"
        "import popcorn.pytest._loop, popcorn.pytest._options\n"
    )
    subprocess.run([sys.executable, "-c", code], check=True)
