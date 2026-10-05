"""Facade support for the plugin: strictness override, last stop, default budget, hook removal,
ring clear."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Coroutine
from typing import cast

import popcorn
import pytest

from .helpers import CALLEE_ELF, DRIVER_ELF, M33_JSON, PREBUILT, ROOT

HOOKS_ELF = PREBUILT / "hooks.elf"
MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = PREBUILT / "model.elf"


async def bounded[T](run: Coroutine[object, object, T]) -> T:
    """`run` with a 60 s wall hang guard, for runs only a default budget bounds."""
    return await asyncio.wait_for(run, 60)


def driver(scenario: str | None = None) -> popcorn.Machine:
    image = popcorn.Image.elf(DRIVER_ELF)
    table = None if scenario is None else image.lookup(f"vt_{scenario}").value
    m = popcorn.Machine(popcorn.load_description(M33_JSON), image=image, vector_table=table)
    m.reset()
    return m


def callee() -> popcorn.Machine:
    m = popcorn.Machine(popcorn.load_description(M33_JSON), image=popcorn.Image.elf(CALLEE_ELF))
    m.reset()
    return m


def hooked() -> popcorn.Machine:
    m = popcorn.Machine(popcorn.load_description(M33_JSON), image=popcorn.Image.elf(HOOKS_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


def model_project() -> popcorn.Project:
    base = popcorn.Project(
        part="POPCORN-M33-TEST",
        description=MODEL_JSON,
        images=[popcorn.Elf(MODEL_ELF)],
        boot=popcorn.Boot(until="ready", budget=10_000),
        strict=False,
    )
    return base.with_configs(variant=base.replace(boot=base.boot.replace(budget=20_000)))


def variant(project: popcorn.Project) -> popcorn.Project:
    return project.config("variant")


def touch(project: popcorn.Project, m: popcorn.Machine) -> popcorn.CallResult:
    async def body() -> popcorn.CallResult:
        await project.boot_machine(m)
        return await m.call("touch_unmodelled", budget=10_000)

    return asyncio.run(body())


def test_strict_peripherals_overrides_the_project_file() -> None:
    project = model_project()
    digest = project.digest
    assert touch(project, project.machine()).stop.reason is popcorn.StopReason.RETURNED
    with pytest.raises(popcorn.ModelError):
        touch(project, project.machine(strict_peripherals=True))
    assert project.digest == digest
    with pytest.raises(popcorn.ModelError):
        touch(project, variant(project).machine(strict_peripherals=True))
    lax = variant(project).machine(strict_peripherals=False)
    assert touch(project, lax).stop.reason is popcorn.StopReason.RETURNED


def test_strict_none_keeps_the_file_strictness() -> None:
    strict = model_project().replace(strict=True)
    with pytest.raises(popcorn.ModelError):
        touch(strict, strict.machine(strict_peripherals=None))
    assert touch(strict, strict.machine(strict_peripherals=False)).stop.reason is (
        popcorn.StopReason.RETURNED
    )


def test_last_stop_follows_runs_steps_and_calls() -> None:
    m = driver()
    assert m.last_stop is None
    stop = asyncio.run(m.run(until="alu_mark"))
    assert m.last_stop == stop
    stepped = asyncio.run(m.step(3))
    assert m.last_stop == stepped
    waited = asyncio.run(m.wait_for(popcorn.Reached("alu_mark"), for_=popcorn.units.ms(1)))
    assert m.last_stop == waited
    m.reset()
    assert m.last_stop is None
    snapshot = m.snapshot()
    asyncio.run(m.run(budget=1000))
    assert m.last_stop is not None
    m.restore(snapshot)
    assert m.last_stop is None


def test_last_stop_is_none_after_a_run_ending_without_a_stop() -> None:
    m = driver()
    asyncio.run(m.run(budget=1000))
    assert m.last_stop is not None

    async def cancelled() -> None:
        with contextlib.suppress(TimeoutError):
            async with asyncio.timeout(0.005):
                await m.run()

    asyncio.run(cancelled())
    assert m.last_stop is None
    asyncio.run(m.run(budget=1000))

    later = Past(m.instructions + 1000)

    async def background() -> None:
        async with m.running():
            await m.wait_for(later)

    asyncio.run(background())
    assert m.last_stop is None


def test_last_stop_of_a_faulting_call() -> None:
    m = callee()
    result = asyncio.run(m.call("fault", budget=1_000))
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert m.last_stop == result.stop


def test_last_stop_of_a_background_fault_and_not_of_a_waiter() -> None:
    m = driver("fault")
    seen: list[popcorn.Stop] = []

    async def body() -> None:
        with pytest.raises(popcorn.FaultError) as caught:
            async with m.running():
                with pytest.raises(popcorn.MachineRunning):
                    _ = m.last_stop
                seen.append(await m.wait_for(popcorn.Reached("alu_mark")))
        assert caught.value.stop is not None
        seen.append(caught.value.stop)

    asyncio.run(body())
    assert seen[0].reason is popcorn.StopReason.FAULT
    assert m.last_stop == seen[1]


class Past:
    """Ready once the machine executed more than `count` instructions."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        return machine.instructions > self.count


def test_a_waiter_inside_running_sets_no_last_stop() -> None:
    m = driver()

    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(Past(1000))

    stop = asyncio.run(body())
    assert stop.reason is popcorn.StopReason.USER
    assert m.last_stop is None


def test_default_budget_bounds_run_wait_for_and_call() -> None:
    m = driver()
    asyncio.run(m.run(until="alu_mark"))
    m.default_budget = 10_000
    start = m.instructions
    stop = asyncio.run(bounded(m.run()))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert 10_000 <= stop.instructions - start < 10_100
    start = m.instructions
    stop = asyncio.run(m.run(budget=500))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert 500 <= stop.instructions - start < 600
    stop = asyncio.run(bounded(m.wait_for(popcorn.Reached("idle_main"))))
    assert stop.reason is popcorn.StopReason.BUDGET
    c = callee()
    c.default_budget = 5_000
    start = c.instructions
    result = asyncio.run(bounded(c.call("forever")))
    assert result.stop.reason is popcorn.StopReason.BUDGET
    assert 5_000 <= result.stop.instructions - start < 5_100
    start = c.instructions
    result = asyncio.run(c.call("forever", budget=500))
    assert result.stop.reason is popcorn.StopReason.BUDGET
    assert 500 <= result.stop.instructions - start < 600


def test_default_budget_none_and_invalid() -> None:
    m = driver()
    assert m.default_budget is None
    m.default_budget = 10
    m.default_budget = None
    start = m.instructions
    assert asyncio.run(m.step(10)).reason is popcorn.StopReason.BUDGET
    assert m.instructions - start == 10
    stop = asyncio.run(m.run(budget=50_000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert stop.instructions - start >= 50_000
    for bad in (0, -5):
        with pytest.raises(ValueError, match="above 0"):
            m.default_budget = bad
    assert m.default_budget is None


def test_default_budget_refuses_other_types() -> None:
    m = driver()
    for bad in (True, 1.5, "10"):
        with pytest.raises(TypeError, match="an int or None"):
            m.default_budget = cast(int, bad)
    assert m.default_budget is None


def test_running_ignores_the_default_budget() -> None:
    m = driver()
    m.default_budget = 10

    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(Past(5000))

    assert asyncio.run(body()).reason is popcorn.StopReason.USER


def test_default_budget_leaves_step_alone() -> None:
    m = driver()
    m.default_budget = 5
    start = m.instructions
    asyncio.run(m.step(50))
    assert m.instructions - start == 50


def install_four(m: popcorn.Machine) -> tuple[list[object], list[object], popcorn.Watch]:
    called: list[object] = []
    returned: list[object] = []

    def skip(c: popcorn.Call) -> popcorn.Return:
        called.append(c)
        return c.returns()

    m.intercept("hal_delay", skip)
    m.script("next_value", [1, 2])
    m.on_return("triple", returned.append)
    watch = m.watch("shared", handler=lambda hit: None)
    return called, returned, watch


def test_remove_hooks_removes_every_hook_and_log() -> None:
    m = hooked()
    called, returned, watch = install_four(m)
    asyncio.run(m.call("run_script", budget=100_000))
    assert len(m.intercept_log("next_value")) == 3
    assert m.remove_hooks() == 4
    assert m.intercept_log("next_value") == []
    spins = m.sym("delay_spins")
    before = m.mem.u32[spins]
    for fn in ("use_delay", "run_script", "use_triple", "write_three"):
        asyncio.run(m.call(fn, budget=1_000_000))
    assert m.mem.u32[spins] == before + 1000
    assert [m.mem.i32[m.sym("results") + 4 * i] for i in range(3)] == [100, 100, 100]
    assert called == [] and returned == [] and watch.hits == []
    assert m.intercept_log("next_value") == []
    assert m.remove_hooks() == 0


def test_remove_hooks_counts_live_hooks_only() -> None:
    m = hooked()
    install_four(m)[2].remove()
    assert m.remove_hooks() == 3


def test_remove_hooks_from_a_handler_is_refused() -> None:
    m = hooked()

    def remove(c: popcorn.Call) -> popcorn.Return:
        m.remove_hooks()
        return c.proceed()

    m.intercept("hal_delay", remove)
    with pytest.raises(popcorn.MachineRunning):
        asyncio.run(m.call("use_delay", budget=1_000_000))


def test_remove_hooks_from_a_predicate_is_refused_with_no_hook_installed() -> None:
    m = driver()
    with pytest.raises(popcorn.MachineRunning):
        asyncio.run(m.run(stop_when=lambda machine: machine.remove_hooks() > 0, budget=5_000_000))


def test_ring_clear_drops_records_and_continues_the_count() -> None:
    m = driver()
    asyncio.run(m.run(budget=100_000))
    assert m.ring.records()
    m.ring.clear()
    assert m.ring.records() == []
    cleared_at = m.instructions
    asyncio.run(m.run(budget=1_000))
    times = [record.time for record in m.ring.records()]
    assert times
    assert min(times) > cleared_at
    assert max(times) <= m.instructions


def test_ring_clear_is_refused_during_a_run() -> None:
    m = driver()

    def clear(c: popcorn.Call) -> popcorn.Return:
        m.ring.clear()
        return c.proceed()

    m.intercept("alu_mark", clear)
    with pytest.raises(popcorn.MachineRunning):
        asyncio.run(m.run(budget=100_000))
