"""Guest function calls: signatures, results, stops, context and interrupts."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.trace import Irq

from .conftest import CALLEE_ELF, run_bounded


def call(
    m: popcorn.Machine,
    fn: str | int,
    *args: object,
    budget: int | None = None,
    stack: popcorn.Stack = popcorn.Stack.CURRENT,
    stack_top: int | None = None,
    interrupts: bool = False,
    reset: bool | popcorn.Snapshot = False,
) -> popcorn.CallResult:
    return asyncio.run(
        m.call(
            fn,
            *args,
            budget=budget,
            stack=stack,
            stack_top=stack_top,
            interrupts=interrupts,
            reset=reset,
        )
    )


@pytest.mark.parametrize(
    ("fn", "args", "value"),
    [
        ("add", (2, 3), 5),
        ("negate", (7,), -7),
        ("ones", (), 0xFFFF_FFFF),
        ("twice64", (0x1_0000_0003,), 0x2_0000_0006),
        ("twice64", (-5,), -10),
        ("mulf", (1.5, 4.0), 6.0),
        ("half", (5.0,), 2.5),
        ("sum6", (1, 2, 3, 4, 5, 6), 91),
        ("mixed", (1, 2.5, 3, 4.5, 5), 16.0),
    ],
)
def test_signatures_from_dwarf_return_the_c_value(
    callee: popcorn.Machine, fn: str, args: tuple[object, ...], value: float
) -> None:
    result = call(callee, fn, *args, budget=100_000)
    assert result.stop.reason is popcorn.StopReason.RETURNED
    assert result.value == value


def test_a_struct_filled_in_the_arena_reads_back_by_field(callee: popcorn.Machine) -> None:
    point = callee.arena.alloc(callee.types["struct point"])
    result = call(callee, "fill", point, 7)
    assert result.stop.reason is popcorn.StopReason.RETURNED
    assert point.var.x.value == 7
    assert point.var.y.value == -7
    assert point.var.scale.value == 1.5


def test_without_dwarf_kinds_come_from_python_types(m33: popcorn.PartDescription) -> None:
    image = popcorn.Image.elf(CALLEE_ELF)
    stripped = popcorn.Machine(m33, image=image)
    stripped.reset()
    add = image.lookup("add").value
    assert asyncio.run(stripped.call(add, 20, 22)).value == 42
    twice = image.lookup("twice64").value
    wide = asyncio.run(stripped.call(twice, popcorn.i64(1 << 33), returns=popcorn.ValueKind.I64))
    assert wide.value == 1 << 34
    mulf = image.lookup("mulf").value
    product = asyncio.run(
        stripped.call(mulf, popcorn.f32(2.0), popcorn.f32(0.25), returns=popcorn.ValueKind.F32)
    )
    assert product.value == 0.5


def test_a_wrong_argument_count_runs_nothing(callee: popcorn.Machine) -> None:
    before = callee.instructions
    with pytest.raises(TypeError, match="add takes 2 arguments, 1 given"):
        call(callee, "add", 1)
    assert callee.instructions == before


def test_an_endless_call_stops_on_budget_with_context_restored(callee: popcorn.Machine) -> None:
    asyncio.run(callee.run(budget=10_000))
    pc, sp = callee.pc, callee.reg.sp
    result = call(callee, "forever", budget=100_000)
    assert result.stop.reason is popcorn.StopReason.BUDGET
    assert result.value == 0
    assert (callee.pc, callee.reg.sp) == (pc, sp)


def test_a_faulting_call_stops_with_the_fault_and_context_restored(
    callee: popcorn.Machine,
) -> None:
    pc = callee.pc
    result = call(callee, "fault", budget=1_000)
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert result.stop.fault is not None
    assert callee.pc == pc


@pytest.mark.parametrize("stack", [popcorn.Stack.CURRENT, popcorn.Stack.PROCESS])
def test_a_call_on_a_booted_machine_leaves_its_loop_running(
    callee: popcorn.Machine, stack: popcorn.Stack
) -> None:
    loops = callee.sym("loops")
    ticks = callee.sym("ticks")
    asyncio.run(callee.run(budget=50_000))
    before = callee.mem.u32[loops], callee.mem.u32[ticks]
    top = 0x200B_0000 if stack is popcorn.Stack.PROCESS else None
    assert call(callee, "add", 1, 2, stack=stack, stack_top=top).value == 3
    asyncio.run(callee.run(budget=50_000))
    after = callee.mem.u32[loops], callee.mem.u32[ticks]
    assert after[0] > before[0]
    assert after[1] > before[1]


def test_interrupts_during_a_call_only_when_asked(callee: popcorn.Machine) -> None:
    asyncio.run(callee.run(budget=50_000))
    masked = call(callee, "wait_flag", budget=20_000)
    assert masked.stop.reason is popcorn.StopReason.BUDGET
    served = call(callee, "wait_flag", budget=20_000, interrupts=True)
    assert served.stop.reason is popcorn.StopReason.RETURNED
    assert served.value > 0


def test_reset_restores_the_baseline_before_each_call(callee: popcorn.Machine) -> None:
    first = call(callee, "bump", reset=True)
    second = call(callee, "bump", reset=True)
    assert first.value == second.value == 1
    third = call(callee, "bump")
    assert third.value == 2
    baseline = callee.snapshot()
    call(callee, "bump")
    assert call(callee, "bump", reset=baseline).value == 3


def test_cancelling_a_call_ends_it(callee: popcorn.Machine) -> None:
    pc = callee.pc

    async def scenario() -> None:
        task = asyncio.create_task(callee.call("forever"))
        await asyncio.sleep(0.005)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert not callee.busy
    assert callee.pc == pc
    assert call(callee, "add", 1, 1).value == 2


def test_a_struct_by_value_and_an_unknown_argument_are_refused(callee: popcorn.Machine) -> None:
    with pytest.raises(NotImplementedError, match="pass structs by pointer"):
        call(callee, "by_value", 0)
    with pytest.raises(TypeError, match="argument 0 of add"):
        call(callee, "add", "two", 3)


def test_the_call_runs_on_the_stack_it_names(callee: popcorn.Machine) -> None:
    process = call(callee, "stack_now", stack=popcorn.Stack.PROCESS, stack_top=0x200B_0000)
    assert process.value == 0x200B_0000
    main = call(callee, "stack_now", stack=popcorn.Stack.MAIN, stack_top=0x200A_0000)
    assert main.value == 0x200A_0000
    current = call(callee, "stack_now")
    assert current.value == callee.reg.sp
    spsel = call(callee, "control_now", stack=popcorn.Stack.PROCESS, stack_top=0x200B_0000)
    assert isinstance(spsel.value, int) and spsel.value & 2
    msp = call(callee, "control_now").value
    assert isinstance(msp, int) and not msp & 2


def test_narrow_parameters_are_range_checked_and_extended(callee: popcorn.Machine) -> None:
    assert call(callee, "low_byte", 255).value == 255
    with pytest.raises(ValueError, match="argument 0 of low_byte is a uint8_t, 300"):
        call(callee, "low_byte", 300)
    with pytest.raises(ValueError, match="uint8_t, -1"):
        call(callee, "low_byte", -1)
    assert call(callee, "neg16", -1).value == 1
    assert call(callee, "neg16", 5).value == -5
    with pytest.raises(ValueError, match="int16_t, 40000"):
        call(callee, "neg16", 40000)


def test_the_abi_override_reads_a_soft_float_function(callee: popcorn.Machine) -> None:
    args = (1, 2.5, 3, 4.5, 5)
    soft = asyncio.run(callee.call("soft_mixed", *args, abi=popcorn.FloatAbi.SOFT))
    assert soft.value == 16.0
    assert asyncio.run(callee.call("soft_mixed", *args)).value != 16.0


def test_calls_work_without_an_arena(m33: popcorn.PartDescription) -> None:
    m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF), arena_base=None)
    m.reset()
    assert call(m, "add", 40, 2).value == 42


def test_a_refused_call_restores_nothing(callee: popcorn.Machine) -> None:
    call(callee, "bump", reset=True)
    call(callee, "bump")
    before = call(callee, "bump").value
    with pytest.raises(TypeError):
        call(callee, "add", 1, reset=True)
    with pytest.raises(popcorn.SymbolError):
        call(callee, "absent", reset=True)
    assert call(callee, "bump").value == before + 1


def test_the_first_reset_call_takes_the_baseline_as_it_is(callee: popcorn.Machine) -> None:
    assert call(callee, "bump").value == 1
    assert call(callee, "bump", reset=True).value == 2
    assert call(callee, "bump", reset=True).value == 2
    callee.set_call_baseline(callee.snapshot())
    assert call(callee, "bump", reset=True).value == 3
    assert call(callee, "bump", reset=True).value == 3


def test_the_arena_allocates_by_type_name_and_by_variable(callee: popcorn.Machine) -> None:
    by_name = callee.arena.alloc("struct point")
    call(callee, "fill", by_name, 3)
    assert by_name.var.y.value == -3
    again = callee.arena.alloc(by_name.var)
    assert again.size == by_name.size
    assert again.address != by_name.address
    with pytest.raises(TypeError):
        callee.arena.alloc("struct absent")


def test_the_fallback_return_address_skips_mapped_memory(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["regions"].append(
        {
            "name": "EXTRA",
            "start": 0x6F00_0000,
            "size": 0x1000,
            "kind": "ram",
            "acc": {"read": True, "write": True, "execute": False},
            "secure": False,
        }
    )
    description = popcorn.parse_description(json.dumps(raw), "extra.json")
    m = popcorn.Machine(description, image=popcorn.Image.elf(CALLEE_ELF), arena_base=None)
    m.reset()
    result = call(m, "add", 1, 2)
    assert result.value == 3
    assert result.stop.pc == 0x6F00_1000


STORE_TWICE = bytes.fromhex("0160 1160 7047")
STORE_AT = 0x200B_8000
SCRATCH = 0x200B_C000
SCS_WRITES = {
    "NVIC_ISER0": (0xE000_E100, 0x10),
    "NVIC_IPR0": (0xE000_E400, 0xA0),
    "VTOR": (0xE000_ED08, 0x2000_0000),
    "SHPR3": (0xE000_ED20, 0x00E0_0000),
    "SYST_RVR": (0xE000_E014, 0x1234),
    "MPU_RNR": (0xE000_ED98, 3),
}


@pytest.fixture
def strict_callee(m33: popcorn.PartDescription) -> popcorn.Machine:
    m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF), strict_peripherals=True)
    m.reset()
    m.mem.write(STORE_AT, STORE_TWICE)
    return m


@pytest.mark.parametrize("register", SCS_WRITES)
@pytest.mark.parametrize("ending", ["returned", "model_error"])
def test_scs_writes_made_in_a_call_stay(
    strict_callee: popcorn.Machine, m33: popcorn.PartDescription, register: str, ending: str
) -> None:
    address, value = SCS_WRITES[register]
    unmodelled = next(p.base for p in m33.peripherals if p.ip == "unmodelled")
    second = SCRATCH if ending == "returned" else unmodelled
    pc, sp = strict_callee.pc, strict_callee.reg.sp
    if ending == "returned":
        result = call(strict_callee, STORE_AT | 1, address, value, second, budget=100)
        assert result.stop.reason is popcorn.StopReason.RETURNED
    else:
        with pytest.raises(popcorn.ModelError):
            call(strict_callee, STORE_AT | 1, address, value, second, budget=100)
    assert (strict_callee.pc, strict_callee.reg.sp) == (pc, sp)
    assert strict_callee.mem.u32[address] == value


def test_a_cyccnt_written_in_a_call_counts_on_from_its_value(
    strict_callee: popcorn.Machine,
) -> None:
    strict_callee.mem.u32[0xE000_EDFC] = 1 << 24
    strict_callee.mem.u32[0xE000_1000] = 1
    start = 0x1000_0000
    result = call(strict_callee, STORE_AT | 1, 0xE000_1004, start, SCRATCH, budget=100)
    assert result.stop.reason is popcorn.StopReason.RETURNED
    assert start <= strict_callee.mem.u32[0xE000_1004] < start + 100


def raising(c: popcorn.Call) -> popcorn.Return:
    raise LookupError(f"handler failed at {c.function:#x}")


def test_a_handler_active_when_a_call_ends_is_neither_resumed_nor_pended(
    callee: popcorn.Machine,
) -> None:
    asyncio.run(callee.run(budget=50_000))
    ticks = callee.mem.u32[callee.sym("ticks")]
    pc, sp = callee.pc, callee.reg.sp
    callee.intercept("SysTick_Handler", raising)
    with pytest.raises(LookupError, match="handler failed"):
        call(callee, "wait_flag", budget=20_000, interrupts=True)
    assert (callee.pc, callee.reg.sp, callee.reg.xpsr & 0x1FF) == (pc, sp, 0)
    systick_active = 1 << 11
    systick_pending = 1 << 26
    assert callee.mem.u32[0xE000_ED24] & systick_active == 0
    assert callee.mem.u32[0xE000_ED04] & systick_pending == 0
    callee.remove_hooks()
    asyncio.run(callee.run(budget=50_000))
    assert callee.mem.u32[callee.sym("ticks")] > ticks


def test_a_fault_that_ends_a_call_keeps_its_status_and_nothing_pending(
    callee: popcorn.Machine,
) -> None:
    asyncio.run(callee.run(budget=50_000))
    ticks = callee.mem.u32[callee.sym("ticks")]
    shcsr = callee.mem.u32[0xE000_ED24]
    result = call(callee, "fault", budget=1_000)
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert result.stop.fault is not None
    assert callee.mem.u32[0xE000_ED28] == result.stop.fault.cfsr != 0
    assert callee.mem.u32[0xE000_ED24] == shcsr
    stop = asyncio.run(callee.run(budget=50_000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert callee.mem.u32[callee.sym("ticks")] > ticks


SVC_THEN_RETURN = bytes.fromhex("00df7047")
SVC_AT = 0x2000_F000
SVCALL = 11


def test_an_svc_left_pending_by_a_call_ending_on_budget_is_dropped(
    hooked: popcorn.Machine,
) -> None:
    hooked.mem.write(SVC_AT, SVC_THEN_RETURN)
    svcall_pended = 1 << 15
    result = call(hooked, SVC_AT | 1, budget=1)
    assert result.stop.reason is popcorn.StopReason.BUDGET
    assert hooked.mem.u32[0xE000_ED24] & svcall_pended == 0
    hooked.ring.clear()
    asyncio.run(hooked.run(budget=200))
    entered = [r.number for r in hooked.ring.records() if isinstance(r, Irq)]
    assert SVCALL not in entered


def test_a_function_pointer_argument_calls_back(callee: popcorn.Machine) -> None:
    start = callee.snapshot()
    assert call(callee, "apply", callee.fn_ptr("negate"), 5).value == -5
    callee.restore(start)
    assert call(callee, "apply", callee.sym("negate"), 5).stop.reason is popcorn.StopReason.FAULT
    assert callee.fn_ptr("negate") == callee.sym("negate") | 1
    assert callee.fn_ptr(callee.symbols["negate"]) == callee.sym("negate") | 1


def test_a_function_pointer_needs_a_function_symbol(callee: popcorn.Machine) -> None:
    with pytest.raises(TypeError, match=r"ticks is not a function symbol.*m\.sym\('ticks'\)"):
        callee.fn_ptr("ticks")
    with pytest.raises(TypeError, match="an address carries no symbol type"):
        callee.fn_ptr(cast(str, 0x0800_1234))
    with pytest.raises(popcorn.SymbolError) as missing:
        callee.fn_ptr("negat")
    assert "negate" in missing.value.close_matches


def test_a_function_pointer_reads_no_machine_while_running(callee: popcorn.Machine) -> None:
    async def scenario() -> int:
        async with callee.running():
            return callee.fn_ptr("negate")

    assert run_bounded(scenario()) == callee.sym("negate") | 1
