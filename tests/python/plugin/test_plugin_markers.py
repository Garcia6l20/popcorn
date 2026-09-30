"""The `popcorn` marker and the config, strict and budget options, FR-PYT-5, FR-PYT-6."""

from __future__ import annotations

import pytest

from .helpers import write_conftest, write_model_project, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

VARIANT = """
[configs.variant]
boot.memory_init = [{ symbol = "alu_rounds", bytes = "07000000" }]
"""

CONFIGS = """
import pytest

def rounds(machine):
    return machine.mem.u32[machine.sym("alu_rounds")]

def test_unmarked(machine, popcorn_project, request):
    expected = 7 if request.config.getoption("popcorn_config") == "variant" else 0
    assert rounds(machine) == expected
    assert popcorn_project.config == request.config.getoption("popcorn_config")

@pytest.mark.popcorn(config="variant")
def test_variant(machine):
    assert rounds(machine) == 7

@pytest.mark.popcorn(config=None)
def test_base(machine):
    assert rounds(machine) == 0

@pytest.mark.popcorn(config="variant")
def test_factory_follows_the_marker(machine_factory):
    assert rounds(machine_factory()) == 7
    assert rounds(machine_factory(config="variant")) == 7
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def test_config_from_marker_and_option(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, extra=VARIANT)
    pytester.makepyfile(CONFIGS)
    result = run(pytester)
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 2 boot(s), 0 from cache"])
    result = run(pytester, "--popcorn-config=variant")
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 2 boot(s), 0 from cache"])


STRICT = """
import popcorn
import pytest

async def touch(machine):
    return await machine.call("touch_unmodelled", budget=10_000)

@pytest.mark.popcorn(strict=False)
async def test_lax(machine):
    await touch(machine)

async def test_unmarked(machine, request):
    if request.config.getoption("popcorn_strict"):
        with pytest.raises(popcorn.ModelError):
            await touch(machine)
    else:
        await touch(machine)

@pytest.mark.popcorn(strict=True)
async def test_strict(machine):
    with pytest.raises(popcorn.ModelError):
        await touch(machine)

@pytest.mark.popcorn(strict=None)
async def test_strict_none(machine_factory, request):
    if request.config.getoption("popcorn_strict"):
        with pytest.raises(popcorn.ModelError):
            await touch(machine_factory())
    else:
        await touch(machine_factory())

@pytest.mark.popcorn(strict=True)
async def test_strict_factory(machine_factory):
    with pytest.raises(popcorn.ModelError):
        await touch(machine_factory())
"""


def test_strict_from_marker_and_option(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_model_project(pytester)
    pytester.makepyfile(STRICT)
    run(pytester).assert_outcomes(passed=5)
    run(pytester, "--popcorn-strict").assert_outcomes(passed=5)


def test_every_strict_test_on_a_reused_machine_raises(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_model_project(pytester)
    pytester.makepyfile("""
        import popcorn
        import pytest

        @pytest.mark.popcorn(strict=True)
        @pytest.mark.parametrize("n", range(2))
        async def test_strict(machine, n):
            with pytest.raises(popcorn.ModelError):
                await machine.call("touch_unmodelled", budget=10_000)
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_marker_is_checked_without_popcorn_fixtures(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.popcorn(budgte=5)
        def test_typo():
            pass

        @pytest.mark.popcorn(budget=5)
        def test_fine():
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(["*@pytest.mark.popcorn has no keyword budgte*"])


BUDGET = """
import asyncio

import popcorn
import pytest

async def test_option(machine, request):
    stop = await asyncio.wait_for(machine.run(), 60)
    assert stop.reason is popcorn.StopReason.BUDGET
    assert machine.default_budget == request.config.getoption("popcorn_budget")

@pytest.mark.popcorn(budget=500)
async def test_marker(machine):
    start = machine.instructions
    stop = await asyncio.wait_for(machine.run(), 60)
    assert stop.reason is popcorn.StopReason.BUDGET
    assert 500 <= stop.instructions - start < 600

@pytest.mark.popcorn(budget=None)
def test_marker_none(machine):
    assert machine.default_budget is None

@pytest.mark.popcorn(budget=700)
def test_factory(machine_factory):
    assert machine_factory().default_budget == 700
    assert machine_factory(booted=False).default_budget is None

def test_next_test_gets_the_option_again(machine):
    assert machine.default_budget == 10_000
"""


def test_budget_from_marker_and_option(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(BUDGET)
    run(pytester, "--popcorn-budget=10000").assert_outcomes(passed=5)


def test_budget_is_reset_after_the_test(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.popcorn(budget=500)
        def test_bounded(machine):
            assert machine.default_budget == 500

        def test_unbounded(machine):
            assert machine.default_budget is None
        """)
    run(pytester).assert_outcomes(passed=2)


def test_the_closest_marker_wins_without_merging(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, extra=VARIANT)
    pytester.makepyfile("""
        import pytest

        pytestmark = pytest.mark.popcorn(config="variant", budget=900)

        def rounds(machine):
            return machine.mem.u32[machine.sym("alu_rounds")]

        def test_module_marker(machine):
            assert (rounds(machine), machine.default_budget) == (7, 900)

        @pytest.mark.popcorn(budget=300)
        def test_function_marker(machine):
            assert (rounds(machine), machine.default_budget) == (0, 300)
        """)
    run(pytester).assert_outcomes(passed=2)


def test_invalid_markers_error_at_setup(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.popcorn("x")
        def test_positional(machine):
            pass

        @pytest.mark.popcorn(speed=1)
        def test_unknown(machine):
            pass

        @pytest.mark.popcorn(budget=-1)
        def test_negative_budget(machine):
            pass

        @pytest.mark.popcorn(budget=True)
        def test_bool_budget(machine):
            pass

        @pytest.mark.popcorn(strict="yes")
        def test_strict_str(machine):
            pass

        @pytest.mark.popcorn(config=3)
        def test_config_int(machine):
            pass

        @pytest.mark.popcorn(config="missing")
        def test_missing_config(machine):
            pass

        def test_fine(machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=7)
    result.stdout.fnmatch_lines_random(
        [
            "*@pytest.mark.popcorn takes keywords only, not the positional ('x',)*",
            "*@pytest.mark.popcorn has no keyword speed*",
            "*@pytest.mark.popcorn budget is an int above 0 or None, not -1*",
            "*@pytest.mark.popcorn budget is an int above 0 or None, not True*",
            "*@pytest.mark.popcorn strict is a bool or None, not 'yes'*",
            "*@pytest.mark.popcorn config is a str or None, not 3*",
            "*ConfigError*missing*",
        ]
    )


@pytest.mark.parametrize("budget", ["0", "-5"])
def test_a_budget_option_not_above_zero_is_a_usage_error(
    pytester: pytest.Pytester, budget: str
) -> None:
    write_conftest(pytester)
    result = run(pytester, f"--popcorn-budget={budget}")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([f"*--popcorn-budget must be above 0, not {budget}*"])
