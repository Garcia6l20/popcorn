"""Linked system fixtures declared with `popcorn.pytest.system`: members booted from their own
descriptions' boot caches and linked."""

from __future__ import annotations

import textwrap
from pathlib import Path

import pytest

from .helpers import ROOT, write_conftest

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")
USART_LL = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "usart_ll.elf"

PROJECTS = """
from pathlib import Path

import popcorn


def project(name: str, budget: int) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        part="STM32U575QGIx",
        description=Path({description!r}),
        images=[popcorn.Elf(Path({elf!r}))],
        name=name,
        boot=popcorn.Boot(until="main", budget=budget),
    )


A = project("a", 1_000_000)
B = project("b", 2_000_000)
"""

PROJECT_CONFTEST = """
import popcorn.pytest

from link_projects import {name}


@popcorn.pytest.machine
def machine():
    return {name}
"""

SYSTEM_CONFTEST = """
import gc
import json
import weakref
from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.units import us

from link_projects import A, B

MEMBERS = []


@popcorn.pytest.system
def system():
    return popcorn.SystemDescription(
        members={"a": A, "b1": B, "b2": B},
        links=[
            popcorn.UartLink("a.USART1", "b1.USART1", latency=us(20), baud=460_800),
            popcorn.UartLink("a.USART2", "b2.USART1", latency=us(20), baud=460_800),
        ],
    )


@pytest.hookimpl(wrapper=True)
def pytest_fixture_setup(fixturedef, request):
    result = yield
    if fixturedef.argname == "system":
        MEMBERS.extend(weakref.ref(m) for m in result.members.values())
    return result


def pytest_sessionfinish(session):
    gc.collect()
    Path("members.json").write_text(
        json.dumps({"members": len(MEMBERS), "alive": sum(ref() is not None for ref in MEMBERS)})
    )
"""

SYSTEM_TESTS = """
import asyncio

import pytest
from popcorn import StopReason
from popcorn.units import VirtualTime, ms


def received(m):
    count = m.mem.u32[m.sym("log_len")]
    words = [m.mem.u32[m.sym("log_buf") + 4 * i] for i in range(min(count, 256))]
    return [word & 0xFFFF for word in words[::2] if word >> 16 == 1]


async def test_members(system):
    members = system.members
    assert list(members) == ["a", "b1", "b2"]
    a, b1, b2 = members.values()
    assert [m.name for m in members.values()] == ["a", "b1", "b2"]
    assert len({id(a), id(b1), id(b2)}) == 3
    assert all(m.pc == m.sym("main") for m in members.values())
    assert a.default_budget is None
    b1.mem.u32[b1.sym("log_len")] = 7
    assert b2.mem.u32[b2.sym("log_len")] == 0
    b1.mem.u32[b1.sym("log_len")] = 0
    a.reg.pc = a.sym("link_tx")
    b1.reg.pc = b1.sym("link_rx")
    b2.reg.pc = b2.sym("link_rx")
    stop = await system.run(for_=ms(3))
    assert (stop.reason, stop.member) == (StopReason.USER, "a")
    frames = system.tap("a.USART1").frames()
    assert len(frames) == 64
    assert all(f.arrival.ps == f.end.ps + system.quantum.ps for f in frames)
    done = await system.run(until=VirtualTime(ms(3).ps))
    assert done.reason is StopReason.TIME
    assert received(b1) == [f.data for f in frames]
    assert received(b2) == []


async def test_again(system):
    assert system.members["b2"].pc == system.members["b2"].sym("main")
"""

A_TEST = """
def test_alone(machine):
    assert machine.pc == machine.sym("main")
"""


@pytest.fixture(scope="module")
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


@pytest.fixture
def layout(u575_json: Path, pytester: pytest.Pytester) -> Path:
    """The rootdir: `a/` and `b/` with their projects, `sys/` with the system of `a` and two `b`
    members, the projects shared through `link_projects.py`. Returns `sys/`."""
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    (pytester.path / "link_projects.py").write_text(
        PROJECTS.format(description=str(u575_json), elf=str(USART_LL))
    )
    for name in ("a", "b"):
        directory = pytester.mkdir(name)
        (directory / "conftest.py").write_text(PROJECT_CONFTEST.format(name=name.upper()))
    (pytester.path / "a" / "test_a.py").write_text(A_TEST)
    system = pytester.mkdir("sys")
    (system / "conftest.py").write_text(SYSTEM_CONFTEST)
    (system / "test_sys.py").write_text(SYSTEM_TESTS)
    return system


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest("-p", "no:asyncio", "--rootdir", str(pytester.path), *args)


def summary(result: pytest.RunResult) -> str:
    return next(
        line for line in result.outlines if line.startswith("popcorn: ") and "key(s)" in line
    )


def members_freed(pytester: pytest.Pytester) -> tuple[int, int]:
    import json

    data = json.loads((pytester.path / "members.json").read_text())
    return data["members"], data["alive"]


def test_members_boot_from_their_projects_and_link(layout: Path, pytester: pytest.Pytester) -> None:
    del layout
    result = run(pytester)
    result.assert_outcomes(passed=3)
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"
    assert members_freed(pytester) == (6, 0)
    again = run(pytester, "-p", "no:randomly")
    again.assert_outcomes(passed=3)
    assert summary(again) == "popcorn: 2 key(s), 0 boot(s), 2 from cache"


def test_command_line_strictness_reaches_every_member(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_strict.py").write_text(
        "def test_strict(system):\n"
        "    assert all(m._strict_peripherals for m in system.members.values())\n"
    )
    result = run(pytester, "--popcorn-strict")
    result.assert_outcomes(passed=4)
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


def test_ring_blocks_reach_every_member(layout: Path, pytester: pytest.Pytester) -> None:
    (layout / "test_blocks.py").write_text(
        "def test_blocks(system):\n"
        "    assert all(m.ring.blocks for m in system.members.values())\n"
    )
    run(pytester, str(layout / "test_blocks.py"), "--popcorn-ring-blocks").assert_outcomes(passed=1)
    (layout / "test_no_blocks.py").write_text(
        "def test_no_blocks(system):\n"
        "    assert not any(m.ring.blocks for m in system.members.values())\n"
    )
    run(pytester, str(layout / "test_no_blocks.py")).assert_outcomes(passed=1)


def test_a_strict_marker_applies_to_the_members_only(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_strict.py").write_text(
        "import pytest\n\n\n"
        "@pytest.mark.popcorn(strict=True, budget=5000)\n"
        "def test_strict(system):\n"
        "    for m in system.members.values():\n"
        "        assert m._strict_peripherals and m.default_budget == 5000\n"
    )
    result = run(pytester, str(layout / "test_strict.py"), "a")
    result.assert_outcomes(passed=2)
    assert summary(result) == "popcorn: 3 key(s), 3 boot(s), 0 from cache"


def test_a_config_marker_fails_a_system_test(layout: Path, pytester: pytest.Pytester) -> None:
    (layout / "test_config.py").write_text(
        "import pytest\n\n\n"
        '@pytest.mark.popcorn(config="other")\n'
        "def test_config(system):\n"
        "    pass\n"
    )
    result = run(pytester, str(layout / "test_config.py"))
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*@pytest.mark.popcorn config is not a marker keyword: *"])


@pytest.mark.parametrize(
    ("fixture", "expected"),
    [
        (None, "*fixture 'system' not found*"),
        (
            "A",
            "*the popcorn fixture system returned MachineDescription, not a "
            "popcorn.SystemDescription*",
        ),
    ],
)
def test_a_missing_or_wrong_system_fails_naming_the_fixture(
    layout: Path, pytester: pytest.Pytester, fixture: str | None, expected: str
) -> None:
    other = pytester.mkdir("other")
    conftest = "import popcorn.pytest\n"
    if fixture is not None:
        conftest += textwrap.dedent(f"""
            from link_projects import {fixture}


            @popcorn.pytest.system
            def system():
                return {fixture}
            """)
    (other / "conftest.py").write_text(conftest)
    (other / "test_other.py").write_text("def test_other(system):\n    pass\n")
    del layout
    result = run(pytester, str(other))
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines([expected])


def test_a_failing_system_test_reports_and_traces_each_member(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_fail.py").write_text(
        "async def test_fail(system):\n" "    system.start()\n" "    assert False\n"
    )
    traces = pytester.path / "traces"
    result = run(pytester, str(layout / "test_fail.py"), f"--popcorn-trace-on-fail={traces}")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines_random(
        [
            "system member a (description=a config=- digest=* strict=False):",
            "system member b1 (description=b config=- digest=* strict=False):",
            "system member b2 (description=b config=- digest=* strict=False):",
            f"trace: {traces}/*test_fail*.system.a.pctr",
            f"trace: {traces}/*test_fail*.system.b1.pctr",
            f"trace: {traces}/*test_fail*.system.b2.pctr",
        ]
    )
    kept = sorted(path.name.rsplit(".", 2)[1] for path in traces.glob("*.pctr"))
    assert kept == ["a", "b1", "b2"]
    assert not list(traces.glob("*.part"))


def test_a_failure_report_shows_each_member_stop_of_the_last_system_run(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_stops.py").write_text(
        "from popcorn import StopReason\n"
        "from popcorn.units import VirtualTime, ms\n\n\n"
        "async def test_stops(system):\n"
        "    a, b1, b2 = system.members.values()\n"
        "    a.reg.pc = a.sym('link_tx')\n"
        "    b1.reg.pc = b1.sym('link_rx')\n"
        "    b2.reg.pc = b2.sym('link_rx')\n"
        "    first = await system.run(for_=ms(3))\n"
        "    assert (first.reason, first.member) == (StopReason.USER, 'a')\n"
        "    done = await system.run(until=VirtualTime(ms(3).ps))\n"
        "    assert done.reason is StopReason.TIME\n"
        "    assert False\n"
    )
    result = run(pytester, str(layout / "test_stops.py"))
    result.assert_outcomes(failed=1)
    for name in ("a", "b1", "b2"):
        result.stdout.fnmatch_lines(
            [
                f"system member {name} (description=* config=- digest=* strict=False):",
                "  stop: TIME at *",
            ],
            consecutive=True,
        )
    result.stdout.no_fnmatch_line("*no last stop*")


def test_a_member_fault_report_shows_the_others_standing_where_the_run_left_them(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_fault.py").write_text(
        "from popcorn import StopReason\n"
        "from popcorn.units import ms\n\n\n"
        "async def test_fault(system):\n"
        "    a = system.members['a']\n"
        "    a.reg.pc = 0xFFFF_FFF0\n"
        "    stop = await system.run(for_=ms(1))\n"
        "    assert (stop.reason, stop.member) == (StopReason.FAULT, 'a')\n"
        "    assert False\n"
    )
    result = run(pytester, str(layout / "test_fault.py"))
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        ["system member a (description=* config=- digest=* strict=False):", "  stop: FAULT at *"],
        consecutive=True,
    )
    for name in ("b1", "b2"):
        result.stdout.fnmatch_lines(
            [
                f"system member {name} (description=* config=- digest=* strict=False):",
                "  no last stop: the machine stands where its last run, if any, left it",
            ],
            consecutive=True,
        )


def test_a_free_run_left_started_is_halted_at_teardown(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_left.py").write_text(
        "async def test_left(system):\n"
        "    for name, m in system.members.items():\n"
        "        m.reg.pc = m.sym('link_rx')\n"
        "    system.start()\n"
    )
    run(pytester, str(layout / "test_left.py")).assert_outcomes(passed=1)
    assert members_freed(pytester) == (3, 0)


def test_workers_run_system_tests(layout: Path, pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    del layout
    result = pytester.runpytest_subprocess(*BASE, "-n", "2")
    result.assert_outcomes(passed=3)


def test_a_member_stop_the_test_never_saw_fails_the_teardown(
    layout: Path, pytester: pytest.Pytester
) -> None:
    (layout / "test_ended.py").write_text(
        "from popcorn.units import ms\n\n\n"
        "async def test_ended(system):\n"
        "    a, b1, b2 = system.members.values()\n"
        "    a.reg.pc = a.sym('link_tx')\n"
        "    b1.reg.pc = b1.sym('link_rx')\n"
        "    b2.reg.pc = b2.sym('link_rx')\n"
        "    system.start()\n"
        "    await b1.elapse(ms(3))\n"
    )
    result = run(pytester, str(layout / "test_ended.py"))
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(
        ["*EmulationError: a: its run in the system's free run stopped with USER*"]
    )
