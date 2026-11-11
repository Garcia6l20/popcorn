# pyright: reportPrivateUsage=false
"""Call bodies behind `Machine.call`."""

from __future__ import annotations

import types
import warnings
from collections.abc import Mapping, Sequence
from dataclasses import replace
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._call import (
    I32_SLOT,
    CallResult,
    HandlerCallWarning,
    Signature,
    Stack,
    argument,
    decode,
    signature_of,
    slot_for,
)
from popcorn._dwarf_views import Var
from popcorn._enums import FloatAbi, StopReason, ValueKind
from popcorn._host import awaited
from popcorn._image import Symbol
from popcorn._machine_checks import FAULT_CONTEXT
from popcorn._machine_events import FIRST_IRQ, SYSTEM_EXCEPTIONS
from popcorn._snapshot import Snapshot
from popcorn._stop import stop_of

if TYPE_CHECKING:
    from popcorn._machine import Machine
    from popcorn.image.dwarf import DebugType


async def call(
    machine: Machine,
    fn: int | str | Symbol,
    *args: object,
    returns: ValueKind | None = None,
    budget: int | None = None,
    privileged: bool = True,
    stack: Stack = Stack.CURRENT,
    stack_top: int | None = None,
    interrupts: bool = False,
    reset: bool | Snapshot = False,
    abi: FloatAbi | None = None,
    from_thread: bool | None = None,
) -> CallResult:
    if from_thread is not None and not isinstance(cast(object, from_thread), bool):
        raise TypeError(f"from_thread is True, False or None, not {from_thread!r}")
    entry = machine.address_of(fn)
    signature = call_signature(machine, fn, entry)
    where = signature.name
    if signature.unsupported is not None:
        raise NotImplementedError(signature.unsupported)
    if signature.params is not None and len(signature.params) != len(args):
        raise TypeError(f"{where} takes {len(signature.params)} arguments, {len(args)} given")
    frame = _core.CallFrame()
    frame.entry = entry
    frame.args = [
        argument(
            value,
            None if signature.params is None else signature.params[index],
            f"argument {index} of {where}",
        )
        for index, value in enumerate(args)
    ]
    result_slot = signature.returns if returns is None else slot_for(returns)
    frame.returns = result_slot.kind
    frame.abi = _core.FloatAbi((abi or abi_at(machine, entry)).value)
    frame.return_sentinel = machine._sentinel
    frame.stack = stack_top
    frame.stack_select = _core.CallStack[stack.value]
    frame.privileged = privileged
    frame.interrupts = interrupts

    def prepare() -> None:
        if isinstance(reset, Snapshot):
            machine.restore(reset)
        elif reset:
            if machine._call_baseline is None:
                machine._call_baseline = machine.snapshot()
            else:
                machine.restore(machine._call_baseline)

    machine._last_stop = None
    outcome = await awaited(
        machine._handle.call_goal(
            frame,
            machine._default_budget if budget is None else budget,
            prepare if reset is not False else None,
            bool(from_thread),
        ),
        lambda raw: cast(_core.CallOutcome, raw),
    )
    stop = stop_of(outcome.stopped)
    if stop.reason is StopReason.FAULT and outcome.began:
        stop = replace(stop, registers=fault_context(machine, outcome.registers))
    if not outcome.began and not stop.detail:
        stop = replace(stop, detail="before thread mode")
    if stop.reason is StopReason.BUDGET and outcome.began and not interrupts:
        hint = masked_hint(machine, outcome.pending_exceptions, outcome.held_exceptions)
        stop = replace(stop, detail=stop.detail or hint)
    machine._last_stop = stop
    bits = outcome.bits
    value = decode(result_slot, bits) if stop.reason is StopReason.RETURNED else 0
    if from_thread is None and outcome.began and outcome.ipsr != 0:
        warnings.warn(
            f"{where} was called with exception {outcome.ipsr} active, which resumes after"
            " it: from_thread=True waits for thread mode, from_thread=False silences this",
            HandlerCallWarning,
            stacklevel=3,
        )
    return CallResult(value, bits, stop, ipsr=outcome.ipsr, began=outcome.began)


def masked_hint(machine: Machine, pending: Sequence[int], held: Sequence[int]) -> str:
    parts: list[str] = []
    if pending:
        parts.append(
            f"{exception_list(machine, pending)} pending: call(interrupts=True) lets them run"
        )
    if held:
        parts.append(
            f"{exception_list(machine, held)} pending, held by the firmware too: PRIMASK,"
            " BASEPRI, FAULTMASK or an active handler masks them, call(interrupts=True) does"
            " not run them"
        )
    if not parts:
        return "interrupts masked by the call: call(interrupts=True) lets them run"
    return f"interrupts masked by the call, {', '.join(parts)}"


def exception_list(machine: Machine, numbers: Sequence[int]) -> str:
    names = {irq.number: irq.name for irq in machine._description.irqs}

    def named(number: int) -> str:
        if number < FIRST_IRQ:
            return f"{SYSTEM_EXCEPTIONS.get(number, 'exception')} ({number})"
        irq = number - FIRST_IRQ
        return f"{names.get(irq, f'IRQ{irq}')} ({irq})"

    shown = ", ".join(named(n) for n in numbers[:4])
    more = f" and {len(numbers) - 4} more" if len(numbers) > 4 else ""
    verb = "is" if len(numbers) == 1 else "are"
    return f"{shown}{more} {verb}"


def fault_context(machine: Machine, values: Sequence[int]) -> Mapping[str, int]:
    read = dict(zip(machine._handle.register_names(), values, strict=True))
    captured = {name: read[name] for name in FAULT_CONTEXT if name in read}
    return types.MappingProxyType(captured)


def call_signature(machine: Machine, fn: int | str | Symbol, entry: int) -> Signature:
    name = fn if isinstance(fn, str) else fn.name if isinstance(fn, Symbol) else f"{fn:#x}"
    for info in reversed(machine._debug_infos()):
        function = info.functions.get(name) if isinstance(fn, str | Symbol) else None
        if function is None:
            function = next((f for f in info.functions.values() if f.address == entry), None)
        if function is not None and function.address == entry:
            return signature_of(info, function)
    return Signature(name, None, I32_SLOT)


def abi_at(machine: Machine, address: int) -> FloatAbi:
    for image in reversed(machine._images):
        if any(s.start <= address < s.start + s.memory_size for s in image.segments):
            return image.abi
    return FloatAbi.SOFT


def type_view(machine: Machine, debug_type: DebugType | str, address: int) -> Var | None:
    for info in reversed(machine._debug_infos()):
        if isinstance(debug_type, str):
            type_id = info.types.get(debug_type)
            if type_id is not None:
                return Var(machine._memory, info, type_id, address, debug_type)
            continue
        for type_id, candidate in info.by_id.items():
            if candidate is debug_type:
                name = next((n for n, t in info.types.items() if t == type_id), "block")
                return Var(machine._memory, info, type_id, address, name)
    return None
