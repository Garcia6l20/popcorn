"""The `popcorn` marker and the strict and budget options, FR-PYT-5, FR-PYT-6."""

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


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def declare(pytester: pytest.Pytester, fixtures: str) -> None:
    """Append `fixtures` to the conftest `write_project` wrote."""
    path = pytester.path / "conftest.py"
    path.write_text(path.read_text() + textwrap.dedent(fixtures))


def test_an_unknown_config_name_fails_naming_the_known_ones(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT)
    pytester.makepyfile("""
        def test_factory(machine_factory, description):
            machine_factory(description, config="nope")
        """)
    result = run(pytester)
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
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
async def test_strict_none(machine_factory, description, request):
    if request.config.getoption("popcorn_strict"):
        with pytest.raises(popcorn.ModelError):
            await touch(machine_factory(description))
    else:
        await touch(machine_factory(description))

@pytest.mark.popcorn(strict=True)
async def test_strict_factory(machine_factory, description):
    with pytest.raises(popcorn.ModelError):
        await touch(machine_factory(description))

@pytest.mark.popcorn(strict=True)
async def test_factory_lax_overrides_strict_marker(machine_factory, description):
    await touch(machine_factory(description, strict=False))

async def test_factory_strict_overrides_lax_default(machine_factory, description):
    with pytest.raises(popcorn.ModelError):
        await touch(machine_factory(description, strict=True))
"""


def test_strict_from_marker_and_option(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_model_project(pytester)
    pytester.makepyfile(STRICT)
    run(pytester).assert_outcomes(passed=7)
    run(pytester, "--popcorn-strict").assert_outcomes(passed=7)


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
    stop = await asyncio.wait_for(machine.run(), 10)
    assert stop.reason is popcorn.StopReason.BUDGET
    assert machine.default_budget == request.config.getoption("popcorn_budget")

@pytest.mark.popcorn(budget=500)
async def test_marker(machine):
    start = machine.instructions
    stop = await asyncio.wait_for(machine.run(), 10)
    assert stop.reason is popcorn.StopReason.BUDGET
    assert 500 <= stop.instructions - start < 600

@pytest.mark.popcorn(budget=None)
def test_marker_none(machine):
    assert machine.default_budget is None

@pytest.mark.popcorn(budget=700)
def test_factory(machine_factory, description):
    assert machine_factory(description).default_budget == 700
    assert machine_factory(description, booted=False).default_budget is None

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


def test_a_marker_config_fails_naming_the_decorator(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT)
    pytester.makepyfile(
        test_function="""
        import pytest

        @pytest.mark.popcorn(config="variant")
        def test_named(machine):
            pass

        @pytest.mark.popcorn(config=None, budget=5)
        def test_none(machine_factory):
            pass

        @pytest.mark.popcorn(config="variant")
        def test_without_fixtures():
            pass
        """,
        test_module="""
        import pytest

        pytestmark = pytest.mark.popcorn(config="variant")

        def test_module(machine):
            pass
        """,
    )
    result = run(pytester)
    result.assert_outcomes(errors=4)
    message = (
        "@pytest.mark.popcorn config is not a marker keyword: set it on the machine fixture, "
        "@popcorn.pytest.machine(config=...), or pass it to "
        "machine_factory(description, config=...)"
    )
    assert result.stdout.str().count(message) == 4
    result.stdout.no_fnmatch_line("*popcorn: * key(s)*")


@pytest.mark.parametrize("budget", ["0", "-5"])
def test_a_budget_option_not_above_zero_is_a_usage_error(
    pytester: pytest.Pytester, budget: str
) -> None:
    write_conftest(pytester)
    result = run(pytester, f"--popcorn-budget={budget}")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([f"*--popcorn-budget must be above 0, not {budget}*"])
