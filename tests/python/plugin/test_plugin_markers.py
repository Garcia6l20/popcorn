"""The `popcorn` marker and the config, strict and budget options, FR-PYT-5, FR-PYT-6."""

from __future__ import annotations

import textwrap

import pytest

from .helpers import write_conftest, write_model_project, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

VARIANT = (
    "variant=BASE.replace(boot=BASE.boot.replace("
    'memory_init=[popcorn.MemoryInit("alu_rounds", bytes.fromhex("07000000"))]))'
)

CONFIGS = """
import pytest

def rounds(machine):
    return machine.mem.u32[machine.sym("alu_rounds")]

def test_unmarked(machine, popcorn_project, request):
    option = request.config.getoption("popcorn_config")
    expected = 7 if option == "variant" else 0
    assert rounds(machine) == expected
    assert popcorn_project.boot.memory_init == ()
    assert len(popcorn_project.config(option).boot.memory_init) == (1 if option == "variant" else 0)

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


def declare(pytester: pytest.Pytester, fixtures: str) -> None:
    """Append `fixtures` to the conftest `write_project` wrote."""
    path = pytester.path / "conftest.py"
    path.write_text(path.read_text() + textwrap.dedent(fixtures))


def test_config_from_marker_and_option(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT, legacy=True)
    pytester.makepyfile(CONFIGS)
    result = run(pytester)
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 2 boot(s), 0 from cache"])
    result = run(pytester, "--popcorn-config=variant")
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 0 boot(s), 2 from cache"])


def test_an_unknown_config_name_fails_naming_the_known_ones(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT)
    pytester.makepyfile("""
        def test_factory(machine_factory):
            machine_factory(config="nope")
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        ["*configs.nope: expected a config of the machine description: variant*"]
    )


def test_an_unknown_marker_or_option_config_fails_naming_the_known_ones(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT, legacy=True)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.popcorn(config="nope")
        def test_marker(machine):
            pass

        def test_option(machine_factory):
            pass
        """)
    marker = run(pytester, "-k", "test_marker")
    marker.assert_outcomes(errors=1, deselected=1)
    marker.stdout.fnmatch_lines(
        ["*configs.nope: expected a config of the machine description: variant*"]
    )
    option = run(pytester, "-k", "test_option", "--popcorn-config=nope")
    option.assert_outcomes(errors=1, deselected=1)
    option.stdout.fnmatch_lines(
        ["*configs.nope: expected a config of the machine description: variant*"]
    )


def test_two_config_names_with_equal_content_share_one_boot(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=f"{VARIANT}, {VARIANT.replace('variant=', 'twin=', 1)}")
    declare(
        pytester,
        """
        @popcorn.pytest.machine(params=["variant", "twin"])
        def configured(request):
            return PROJECT.config(request.param)
        """,
    )
    pytester.makepyfile("""
        SEEN = []

        def test_config(configured):
            SEEN.append(configured)
            assert configured is SEEN[0]
            assert configured.mem.u32[configured.sym("alu_rounds")] == 7
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


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
    write_model_project(pytester)
    pytester.makepyfile("""
        import popcorn
        import pytest

        pytestmark = pytest.mark.popcorn(strict=True, budget=900)

        async def touch(machine):
            return await machine.call("touch_unmodelled", budget=10_000)

        async def test_module_marker(machine):
            assert machine.default_budget == 900
            with pytest.raises(popcorn.ModelError):
                await touch(machine)

        @pytest.mark.popcorn(budget=300)
        async def test_function_marker(machine):
            assert machine.default_budget == 300
            await touch(machine)
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

        def test_fine(machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=5)
    result.stdout.fnmatch_lines_random(
        [
            "*@pytest.mark.popcorn takes keywords only, not the positional ('x',)*",
            "*@pytest.mark.popcorn has no keyword speed*",
            "*@pytest.mark.popcorn budget is an int above 0 or None, not -1*",
            "*@pytest.mark.popcorn budget is an int above 0 or None, not True*",
            "*@pytest.mark.popcorn strict is a bool or None, not 'yes'*",
        ]
    )


def test_invalid_marker_configs_error_at_setup(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, legacy=True)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.popcorn(config=3)
        def test_config_int(machine):
            pass

        @pytest.mark.popcorn(config="missing")
        def test_missing_config(machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=2)
    result.stdout.fnmatch_lines_random(
        ["*@pytest.mark.popcorn config is a str or None, not 3*", "*ConfigError*missing*"]
    )


@pytest.mark.parametrize("budget", ["0", "-5"])
def test_a_budget_option_not_above_zero_is_a_usage_error(
    pytester: pytest.Pytester, budget: str
) -> None:
    write_conftest(pytester)
    result = run(pytester, f"--popcorn-budget={budget}")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([f"*--popcorn-budget must be above 0, not {budget}*"])
