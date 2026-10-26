"""The `popcorn.pytest.machine` and `popcorn.pytest.system` decorators: fixtures declared by a
function returning a description, one reused machine per fixture name and key."""

from __future__ import annotations

import os
import pwd
import re
import textwrap
from pathlib import Path

import pytest

from .helpers import (
    BOARD_ELF,
    CALLEE_ELF,
    DRIVER_ELF,
    cov_args,
    write_conftest,
    write_model_project,
    write_project,
    write_project_conftest,
)
from .test_plugin_system import PROJECTS, USART_LL

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")
ONE_BOOT = "popcorn: 1 key(s), 1 boot(s), 0 from cache"

PAIR = """
import popcorn.pytest


@popcorn.pytest.machine
def first():
    return PROJECT


@popcorn.pytest.machine()
def second():
    return PROJECT
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def declare(pytester: pytest.Pytester, fixtures: str) -> None:
    """Append `fixtures` to the conftest `write_project` or `write_model_project` wrote."""
    path = pytester.path / "conftest.py"
    path.write_text(path.read_text() + textwrap.dedent(fixtures))


def write_pair(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    declare(pytester, PAIR)


def test_two_fixtures_of_one_description_are_two_machines_of_one_boot(
    pytester: pytest.Pytester,
) -> None:
    write_pair(pytester)
    pytester.makepyfile("""
        import pytest

        SEEN = {}

        @pytest.mark.parametrize("n", range(3))
        async def test_pair(first, second, n):
            assert first is not second
            assert (first.name, second.name) == ("first", "second")
            assert first.pc == second.pc == first.sym("alu_mark")
            assert SEEN.setdefault("first", first) is first
            assert SEEN.setdefault("second", second) is second
            rounds = first.sym("alu_rounds")
            assert first.mem.u32[rounds] == second.mem.u32[rounds] == 0
            await first.call("alu_mark")
            assert (first.mem.u32[rounds], second.mem.u32[rounds]) == (1, 0)
        """)
    result = run(pytester)
    result.assert_outcomes(passed=3)
    result.stdout.fnmatch_lines([ONE_BOOT])


COUNTED_BUILDS = """
    import pytest
    from popcorn.pytest._session import PopcornSession

    BUILDS = []

    @pytest.fixture(scope="module", autouse=True)
    def counted():
        build = PopcornSession.build

        def counting(self, key, devices=None, name=None):
            BUILDS.append(name)
            return build(self, key, devices, name)

        PopcornSession.build = counting
        yield
        PopcornSession.build = build
    """


def test_the_first_fixture_of_a_key_takes_its_boot_machine(pytester: pytest.Pytester) -> None:
    write_pair(pytester)
    pytester.makepyfile(COUNTED_BUILDS + """
    def test_one(first, _popcorn_session):
        (booted,) = _popcorn_session._booted.values()
        assert booted.machine is first
        assert first.name == "first"
        assert BUILDS == [None]
        assert _popcorn_session.booted_machines() == [first]

    def test_two(first, second, _popcorn_session):
        assert BUILDS == [None, "second"]
        assert _popcorn_session.booted_machines() == [first, second]
        assert first is not second
        assert second.name == "second"
    """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines([ONE_BOOT])


def test_a_fixture_takes_the_boot_machine_of_a_key_the_factory_booted(
    pytester: pytest.Pytester,
) -> None:
    write_pair(pytester)
    pytester.makepyfile(COUNTED_BUILDS + """
    def test_factory(machine_factory, description):
        made = machine_factory(description)
        assert made.name == "POPCORN-M33-TEST"
        assert BUILDS == [None, None]

    def test_fixture(first, _popcorn_session):
        (booted,) = _popcorn_session._booted.values()
        assert booted.machine is first
        assert first.name == "first"
        assert BUILDS == [None, None]
        assert first.pc == first.sym("alu_mark")
    """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines([ONE_BOOT])


def test_a_spoiled_machine_is_rebuilt_and_a_parked_one_reused(pytester: pytest.Pytester) -> None:
    write_pair(pytester)
    pytester.makepyfile(f"""
        import popcorn

        SEEN = []

        def test_loads_another_image(first):
            SEEN.append(first)
            first.load(popcorn.Image.elf({str(CALLEE_ELF)!r}))

        def test_gets_a_clean_machine(first):
            assert first is not SEEN[0]
            assert first.name == "first"
            assert len(first.images) == 1
            assert first.pc == first.sym("alu_mark")

        async def test_leaves_a_run_behind(first):
            SEEN.append(first)
            context = first.running()
            SEEN.append(context)
            await context.__aenter__()
            assert first.busy

        def test_after_a_busy_machine(first):
            assert first is SEEN[1]
            assert not first.busy
        """)
    result = run(pytester, "-W", "ignore")
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines([ONE_BOOT])


def test_the_marker_budget_and_errors_name_the_fixture(pytester: pytest.Pytester) -> None:
    write_pair(pytester)
    pytester.makepyfile("""
        import popcorn
        import pytest

        @pytest.mark.popcorn(budget=1000)
        async def test_named(first, second):
            assert first.default_budget == second.default_budget == 1000
            for machine in (first, second):
                with pytest.raises(popcorn.EmulationError, match=rf"^{machine.name}: .*BUDGET"):
                    await machine.wait_until(lambda m: False)

        def test_unmarked(first, second):
            assert first.default_budget is None
            assert second.default_budget is None
        """)
    run(pytester).assert_outcomes(passed=2)


def test_the_marker_strict_applies_to_every_declared_machine(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_model_project(pytester)
    declare(pytester, PAIR)
    pytester.makepyfile("""
        import popcorn
        import pytest

        async def touch(machine):
            return await machine.call("touch_unmodelled", budget=10_000)

        @pytest.mark.popcorn(strict=True)
        async def test_strict(first, second):
            for machine in (first, second):
                with pytest.raises(popcorn.ModelError):
                    await touch(machine)

        async def test_lax(first, second):
            await touch(first)
            await touch(second)
        """)
    run(pytester).assert_outcomes(passed=2)


VARIANT = (
    "variant=BASE.replace(boot=BASE.boot.replace("
    'memory_init=[popcorn.MemoryInit("alu_rounds", bytes.fromhex("07000000"))]))'
)


def test_the_config_is_set_on_the_decorator(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT)
    declare(
        pytester,
        """
        import popcorn.pytest

        @popcorn.pytest.machine
        def base():
            return PROJECT

        @popcorn.pytest.machine(config="variant")
        def variant():
            return PROJECT

        @popcorn.pytest.machine(config="nope")
        def unknown():
            return PROJECT
        """,
    )
    pytester.makepyfile("""
        import pytest

        def rounds(machine):
            return machine.mem.u32[machine.sym("alu_rounds")]

        def test_configs(base, variant):
            assert (rounds(base), rounds(variant)) == (0, 7)

        def test_unknown(unknown):
            pass

        @pytest.mark.popcorn(config="variant")
        def test_marker_config(base):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(passed=1, errors=2)
    result.stdout.fnmatch_lines_random(
        [
            "*ConfigError*nope*variant*",
            "*@pytest.mark.popcorn config is not a marker keyword: set it on the machine fixture, "
            "@popcorn.pytest.machine(config=...)*",
        ]
    )


def test_the_function_is_a_fixture(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs=VARIANT)
    declare(
        pytester,
        """
        import asyncio

        import popcorn.pytest
        import pytest

        SEEN = []

        @pytest.fixture
        def chosen():
            return "variant"

        @popcorn.pytest.machine
        def asks(tmp_path, request, chosen, unused=3):
            SEEN.append((tmp_path.is_dir(), request.fixturename, chosen, unused))
            return PROJECT.config(chosen)

        @popcorn.pytest.machine
        async def awaited(request):
            await asyncio.sleep(0)
            return PROJECT

        @popcorn.pytest.machine(params=[None, "variant"], ids=["base", "variant"])
        def each(request):
            return PROJECT.config(request.param)
        """,
    )
    pytester.makepyfile("""
        import asyncio

        from conftest import SEEN

        def rounds(machine):
            return machine.mem.u32[machine.sym("alu_rounds")]

        def test_asks(asks):
            assert SEEN == [(True, "asks", "variant", 3)]
            assert asks.name == "asks"
            assert rounds(asks) == 7

        async def test_awaited(awaited):
            assert awaited.name == "awaited"
            assert awaited.pc == awaited.sym("alu_mark")
            await awaited.call("alu_mark")

        def test_each(each, request):
            expected = 7 if request.node.callspec.id == "variant" else 0
            assert rounds(each) == expected
        """)
    result = run(pytester, "-v")
    result.assert_outcomes(passed=4)
    result.stdout.fnmatch_lines_random(
        ["*test_each[[]base[]] PASSED*", "*test_each[[]variant[]] PASSED*"]
    )


def test_an_async_function_boots_its_key_on_the_running_loop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    declare(
        pytester,
        """
        import popcorn.pytest

        @popcorn.pytest.machine
        async def awaited():
            return PROJECT
        """,
    )
    pytester.makepyfile("""
        def test_sync(awaited):
            assert awaited.pc == awaited.sym("alu_mark")

        async def test_async(awaited):
            assert awaited.pc == awaited.sym("alu_mark")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines([ONE_BOOT])


def test_the_report_and_traces_name_each_machine_by_fixture(pytester: pytest.Pytester) -> None:
    write_pair(pytester)
    pytester.makepyfile("""
        async def test_fails(first, second):
            await first.run(budget=1000)
            assert False
        """)
    traces = pytester.path / "traces"
    result = run(pytester, f"--popcorn-trace-on-fail={traces}")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        [
            "*- popcorn -*",
            "first (config=- digest=* strict=False):",
            "  stop: BUDGET at *",
            "*",
            "second (config=- digest=* strict=False):",
        ]
    )
    kept = sorted(path.name.rsplit(".", 2)[1] for path in traces.glob("*.pctr"))
    assert kept == ["first", "second"]


def test_a_failed_boot_is_reported_under_the_fixture_name(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until="never_there")
    declare(pytester, PAIR)
    pytester.makepyfile("""
        def test_one(first):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*- popcorn -*", "first (config=- digest=* strict=False):"])


def test_a_failed_async_boot_is_reported_under_the_fixture_name(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    write_project(pytester, until="never_there")
    declare(
        pytester,
        """
        import popcorn.pytest


        @popcorn.pytest.machine
        async def broken():
            return PROJECT
        """,
    )
    pytester.makepyfile("""
        def test_two(broken):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*- popcorn -*", "broken (config=- digest=* strict=False):"])


@pytest.mark.parametrize("decorator", ["machine", "system"])
@pytest.mark.parametrize("form", ["def", "async def"])
def test_a_generator_function_is_refused_at_declaration(
    pytester: pytest.Pytester, decorator: str, form: str
) -> None:
    write_conftest(pytester)
    write_project(pytester)
    declare(
        pytester,
        f"""
        import popcorn.pytest


        @popcorn.pytest.{decorator}
        {form} controller():
            yield PROJECT
        """,
    )
    pytester.makepyfile("""
        def test_one(controller):
            pass
        """)
    result = run(pytester)
    assert result.ret != 0
    text = result.stdout.str() + result.stderr.str()
    assert f"TypeError: @popcorn.pytest.{decorator} takes a function returning" in text
    assert "not a generator, return the description instead of yielding it: controller" in text


def test_coverage_counts_declared_machines(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, until=None, elf=CALLEE_ELF)
    declare(pytester, PAIR)
    pytester.makepyfile(f"""
        import popcorn

        async def test_spoils(first):
            await first.call("negate", 1)
            first.load(popcorn.Image.elf({str(DRIVER_ELF)!r}))

        async def test_after(first, second):
            await first.call("negate", 2)
            await second.call("negate", 3)
        """)
    result = pytester.runpytest(*NO_ASYNCIO, "-p", "no:xdist", *cov_args("a.info"))
    result.assert_outcomes(passed=2)
    text = (pytester.path / "a.info").read_text()
    assert re.search(r"^FNDA:3,negate$", text, re.M)


def test_declared_machines_under_xdist(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_pair(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.parametrize("n", range(8))
        async def test_pair(first, second, n):
            assert first is not second
            assert (first.name, second.name) == ("first", "second")
            await first.call("alu_mark")
            assert first.mem.u32[first.sym("alu_rounds")] == 1
            assert second.mem.u32[second.sym("alu_rounds")] == 0
        """)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", "2")
    result.assert_outcomes(passed=8)


SYSTEMS = """
import popcorn
import popcorn.pytest
from popcorn.units import us

from link_projects import A, B


def pair():
    return popcorn.SystemDescription(
        members={"a": A, "b": B},
        links=[popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )


@popcorn.pytest.system
def left():
    return pair()


@popcorn.pytest.system
async def right():
    return pair()


@popcorn.pytest.machine
def wrong_machine():
    return pair()


@popcorn.pytest.system
def wrong_system():
    return A
"""


@pytest.fixture
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


def write_systems(pytester: pytest.Pytester, u575_json: Path) -> None:
    write_conftest(pytester, SYSTEMS)
    (pytester.path / "link_projects.py").write_text(
        PROJECTS.format(description=str(u575_json), elf=str(USART_LL))
    )


def run_systems(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, "--rootdir", str(pytester.path), *args)


def test_a_wrong_description_type_fails_naming_it(
    u575_json: Path, pytester: pytest.Pytester
) -> None:
    write_systems(pytester, u575_json)
    pytester.makepyfile("""
        def test_machine(wrong_machine):
            pass

        async def test_system(wrong_system):
            pass
        """)
    result = run_systems(pytester)
    result.assert_outcomes(errors=2)
    result.stdout.fnmatch_lines_random(
        [
            "*the popcorn fixture wrong_machine returned SystemDescription, not a "
            "popcorn.MachineDescription",
            "*the popcorn fixture wrong_system returned MachineDescription, not a "
            "popcorn.SystemDescription",
        ]
    )


def test_two_systems_in_one_test(u575_json: Path, pytester: pytest.Pytester) -> None:
    write_systems(pytester, u575_json)
    pytester.makepyfile("""
        async def test_two(left, right):
            assert left is not right
            assert list(left.members) == list(right.members) == ["a", "b"]
            assert [m.name for m in left.members.values()] == ["a", "b"]
            assert [m.name for m in right.members.values()] == ["a", "b"]
            assert left.members["a"] is not right.members["a"]
            assert False
        """)
    traces = pytester.path / "traces"
    result = run_systems(pytester, f"--popcorn-trace-on-fail={traces}")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines_random(
        [
            "left member a (description=a *",
            "left member b (description=b *",
            "right member a (description=a *",
            "right member b (description=b *",
        ]
    )
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 2 boot(s), 0 from cache"])
    kept = sorted(".".join(path.name.split(".")[-3:-1]) for path in traces.glob("*.pctr"))
    assert kept == ["left.a", "left.b", "right.a", "right.b"]


def test_a_pty_spoils_the_declared_machine(
    u575_json: Path, pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("HOME", pwd.getpwuid(os.getuid()).pw_dir)
    write_conftest(pytester)
    write_project_conftest(
        pytester.path,
        f'popcorn.MachineDescription(part="STM32U575QGIx", '
        f"images=[popcorn.Elf(Path({str(BOARD_ELF)!r}))], "
        'boot=popcorn.Boot(until="ready", budget=1_000_000))',
    )
    declare(pytester, PAIR)
    pytester.makepyfile("""
        SEEN = []

        def test_opens_a_pty(first):
            SEEN.append(first)
            assert first.board.pty("USART1")

        def test_gets_a_new_machine(first):
            assert first is not SEEN[0]
            assert first.name == "first"
            assert first.board.ptys == {}
            assert first.pc == first.sym("ready")
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines([ONE_BOOT])
