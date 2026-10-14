"""`_unwind.return_point`, the step-out target, over hand-assembled Thumb code."""

from __future__ import annotations

from collections.abc import Mapping

from popcorn._unwind import Functions, ReturnPoint, Via, return_point

F = 0x1000
G = 0x2000
SP = 0x2000_0F00
CALL_A = G + 0x8
CALL_B = G + 0xC
CALLER = {
    G: 0xB500,
    G + 2: 0xBF00,
    G + 4: 0xF000,
    G + 6: 0xF800,
    G + 8: 0xF000,
    G + 10: 0xF800,
    G + 12: 0xBD00,
}


def point(code: Mapping[int, int], stack: Mapping[int, int], pc: int) -> ReturnPoint | None:
    halfwords = {**CALLER, **code}

    def read32(address: int) -> int | None:
        if address in stack:
            return stack[address]
        low, high = halfwords.get(address), halfwords.get(address + 2)
        if low is None and high is None:
            return None
        return (low or 0) | (high or 0) << 16

    end = max(code) + 2
    functions = Functions([("f", F, end - F), ("g", G, 14)])
    return return_point(read32, functions, pc, CALL_B | 1, SP)


SHRINK_WRAPPED = {
    F: 0xB110,
    F + 2: 0xB510,
    F + 4: 0x2001,
    F + 6: 0xBD10,
    F + 8: 0x2000,
    F + 10: 0x4770,
}
STACK = {SP + 4: CALL_A | 1}


def test_a_pc_in_the_prologue_body_returns_through_the_saved_lr() -> None:
    assert point(SHRINK_WRAPPED, STACK, F + 4) == ReturnPoint(CALL_A, SP + 8, Via.PROLOGUE)


def test_a_pc_on_an_epilogue_returns_through_it() -> None:
    assert point(SHRINK_WRAPPED, STACK, F + 6) == ReturnPoint(CALL_A, SP + 8, Via.PROLOGUE)
    assert point(SHRINK_WRAPPED, STACK, F + 10) == ReturnPoint(CALL_B, SP, Via.PROLOGUE)


def test_a_pc_in_an_early_exit_after_the_epilogue_is_refused() -> None:
    assert point(SHRINK_WRAPPED, STACK, F + 8) is None


def test_a_pc_after_a_stack_allocation_by_register_is_refused() -> None:
    allocating = {F: 0xB580, F + 2: 0xEBAD, F + 4: 0x0D03, F + 6: 0x2000, F + 8: 0xBD80}
    assert point(allocating, STACK, F + 6) is None
    assert point(allocating, STACK, F + 2) == ReturnPoint(CALL_A, SP + 8, Via.PROLOGUE)


def test_a_pc_after_a_vpop_from_an_odd_s_register_is_refused() -> None:
    popping = {F: 0xB510, F + 2: 0xECFD, F + 4: 0x8A02, F + 6: 0x2000, F + 8: 0xBD10}
    assert point(popping, STACK, F + 6) is None
