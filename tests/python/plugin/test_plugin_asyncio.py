"""Async fixtures on the plugin's loop, and coexistence with pytest-asyncio."""

from __future__ import annotations

import pytest

from .helpers import RECORD_LOOPS, read_loops, write_conftest

pytest_plugins = ["pytester"]

ASYNCIO = ("-o", "asyncio_default_fixture_loop_scope=function")


def test_async_fixtures_run_on_the_owned_loop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester, RECORD_LOOPS)
    pytester.makepyfile("""
        import asyncio
        from pathlib import Path

        import pytest

        from conftest import record

        CALLS = []

        @pytest.fixture(scope="session")
        async def shared():
            record("session", asyncio.get_running_loop())
            CALLS.append("shared")
            return "shared"

        @pytest.fixture
        async def value():
            await asyncio.sleep(0)
            return 41

        @pytest.fixture
        async def resource():
            record("generator", asyncio.get_running_loop())
            yield "open"
            await asyncio.sleep(0)
            Path("teardown.txt").write_text("closed")

        async def test_one(shared, value, resource):
            record("one", asyncio.get_running_loop())
            assert (shared, value, resource) == ("shared", 41, "open")

        async def test_two(shared):
            record("two", asyncio.get_running_loop())
            assert CALLS == ["shared"]

        def test_sync_test_gets_an_async_fixture(value):
            assert value == 41

        class TestMethods:
            @pytest.fixture
            async def bound(self):
                self.seen = True
                return "bound"

            async def test_method_fixture_binds_the_instance(self, bound):
                assert bound == "bound"
                assert self.seen

            async def test_each_instance_is_bound(self, bound):
                assert bound == "bound"
                assert self.seen
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=5)
    loops = read_loops(pytester)
    assert len({loops.ids[name] for name in ("session", "generator", "one", "two")}) == 1
    assert loops.started
    assert loops.closed["one"]
    assert (pytester.path / "teardown.txt").read_text() == "closed"


def test_async_fixture_errors_reach_the_test(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.fixture
        async def broken():
            raise RuntimeError("fixture broke")

        @pytest.fixture
        async def twice():
            yield 1
            yield 2

        async def test_setup_error(broken):
            pass

        async def test_teardown_error(twice):
            pass
        """)
    result = pytester.runpytest("-p", "no:asyncio")
    result.assert_outcomes(passed=1, errors=2)
    result.stdout.fnmatch_lines(
        [
            "*RuntimeError: fixture broke*",
            "*ValueError: the async generator fixture twice yielded twice*",
        ]
    )


def test_context_variables_follow_fixtures_and_stay_in_their_test(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import contextvars

        import pytest

        FROM_FIXTURE = contextvars.ContextVar("from_fixture", default="unset")
        FROM_GENERATOR = contextvars.ContextVar("from_generator", default="unset")
        FROM_TEST = contextvars.ContextVar("from_test", default="unset")

        @pytest.fixture
        async def sets():
            FROM_FIXTURE.set("fixture")

        @pytest.fixture
        async def generator_sets():
            FROM_GENERATOR.set("generator")
            yield
            assert FROM_GENERATOR.get() == "generator"

        async def test_sees_the_fixture_variables(sets, generator_sets):
            assert FROM_FIXTURE.get() == "fixture"
            assert FROM_GENERATOR.get() == "generator"
            FROM_TEST.set("test")

        def test_sync_sees_them_too(sets):
            assert FROM_FIXTURE.get() == "fixture"

        async def test_next_sees_nothing():
            assert FROM_FIXTURE.get() == "unset"
            assert FROM_GENERATOR.get() == "unset"
            assert FROM_TEST.get() == "unset"

        def test_sync_next_sees_nothing():
            assert (FROM_FIXTURE.get(), FROM_GENERATOR.get(), FROM_TEST.get()) == ("unset",) * 3
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=4)


def test_strict_mode_splits_marked_and_unmarked_tests(pytester: pytest.Pytester) -> None:
    pytest.importorskip("pytest_asyncio")
    write_conftest(pytester, RECORD_LOOPS)
    pytester.makepyfile("""
        import asyncio

        import pytest
        import pytest_asyncio

        from conftest import record

        @pytest_asyncio.fixture
        async def for_marked():
            record("fixture of marked", asyncio.get_running_loop())
            return 1

        @pytest_asyncio.fixture
        async def for_unmarked():
            record("fixture of unmarked", asyncio.get_running_loop())
            return 2

        @pytest.fixture
        async def plain():
            record("plain fixture", asyncio.get_running_loop())
            return 3

        async def test_unmarked(plain):
            record("unmarked", asyncio.get_running_loop())
            assert plain == 3

        @pytest.mark.asyncio
        async def test_marked(for_marked):
            record("marked", asyncio.get_running_loop())
            assert for_marked == 1

        async def test_unmarked_with_a_pytest_asyncio_fixture(for_unmarked):
            record("reverse", asyncio.get_running_loop())
            assert for_unmarked == 2
        """)
    pytester.runpytest(*ASYNCIO).assert_outcomes(passed=3)
    ids = read_loops(pytester).ids
    assert ids["unmarked"] == ids["plain fixture"] == ids["reverse"]
    assert ids["marked"] != ids["unmarked"]
    assert ids["fixture of marked"] == ids["marked"]
    assert ids["fixture of unmarked"] != ids["reverse"]


def test_auto_mode_never_starts_the_owned_loop(pytester: pytest.Pytester) -> None:
    pytest.importorskip("pytest_asyncio")
    write_conftest(pytester, RECORD_LOOPS)
    pytester.makepyfile("""
        import asyncio

        import pytest

        from conftest import record

        @pytest.fixture
        async def plain():
            record("plain fixture", asyncio.get_running_loop())
            return 3

        @pytest.fixture
        async def generator():
            yield 4

        async def test_auto(plain, generator):
            record("auto", asyncio.get_running_loop())
            assert (plain, generator) == (3, 4)

        def test_sync(generator):
            assert generator == 4
        """)
    pytester.runpytest("-o", "asyncio_mode=auto", *ASYNCIO).assert_outcomes(passed=2)
    loops = read_loops(pytester)
    assert not loops.started
    assert loops.ids["auto"] == loops.ids["plain fixture"]
