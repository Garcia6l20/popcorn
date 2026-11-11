# pyright: reportPrivateUsage=false
"""Hook installation and stub bodies behind the `Machine` hook methods."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._call import decode
from popcorn._enums import StopReason
from popcorn._errors import MachineBusy, MachineRunning
from popcorn._hooks import Intercept, LogEntry, Return, Returned
from popcorn._host import awaited
from popcorn._image import Symbol
from popcorn._machine_events import (
    Call,
    InterceptHandler,
    Native,
    ReturnHandler,
    error_prefix,
    stop_error,
)
from popcorn._stop import stop_of
from popcorn.units import VirtualDuration

if TYPE_CHECKING:
    from popcorn._machine import Machine


def install_intercept(
    machine: Machine, fn: int | str | Symbol, handler: InterceptHandler, once: bool
) -> Intercept:
    machine._require_stopped()
    entry = machine.address_of(fn)
    signature = machine._signature(fn, entry)
    log = machine._logs.setdefault(entry, [])

    def native(view: _core.InterceptCall) -> _core.InterceptResult:
        call = Call(machine, view, signature)
        with machine._as_handler():
            args = call.args_typed() if signature.params is not None else ()
            reply = handler(call)
        record = LogEntry(call.occurrence, args)
        if reply.action == "returns":
            record.value, record.returned = reply.value, True
        log.append(record)
        return reply.native

    keys: list[int] = []
    hooks = [machine._handle.add_intercept(entry, machine._relay(native, keys), once)]
    if signature.params is not None:
        kinds = [p.kind for p in signature.params]

        def returned(done: _core.ReturnedCall) -> None:
            pending = next((r for r in reversed(log) if not r.returned), None)
            if pending is not None:
                pending.value = None if signature.void else decode(signature.returns, done.value)
                pending.returned = True

        hooks.append(
            machine._handle.add_return_hook(
                entry, kinds, signature.returns.kind, machine._relay(returned, keys)
            )
        )
    return machine._keep(Intercept(hooks, entry, log, machine._callback_table, keys))


def add_return_hook(
    machine: Machine, fn: int | str | Symbol, handler: ReturnHandler
) -> tuple[_core.Hook, Intercept]:
    entry, kinds, returns, returned_of = return_decoder(machine, fn)

    def native(done: _core.ReturnedCall) -> None:
        returned = returned_of(done.occurrence, done.args, done.value, done.instructions)
        with machine._as_handler():
            handler(returned)

    keys: list[int] = []
    hook = machine._handle.add_return_hook(entry, kinds, returns, machine._relay(native, keys))
    return hook, machine._keep(Intercept([hook], entry, [], machine._callback_table, keys))


def add_return_log(
    machine: Machine, fn: int | str | Symbol, log: _core.ReturnLog
) -> tuple[_core.Hook, Intercept, Callable[[_core.LoggedReturn], Returned]]:
    entry, kinds, returns, returned_of = return_decoder(machine, fn)
    hook = machine._handle.add_return_log(entry, kinds, returns, log)

    def decoded(logged: _core.LoggedReturn) -> Returned:
        return returned_of(logged.occurrence, logged.args, logged.value, logged.instructions)

    return hook, machine._keep(Intercept([hook], entry, [], machine._callback_table, [])), decoded


def return_decoder(machine: Machine, fn: int | str | Symbol) -> tuple[
    int,
    list[_core.ValueKind],
    _core.ValueKind,
    Callable[[int, Sequence[int], int, int], Returned],
]:
    machine._require_stopped()
    entry = machine.address_of(fn)
    signature = machine._signature(fn, entry)
    if signature.params is None and signature.unsupported is None:
        raise TypeError(f"{signature.name} has no DWARF signature for on_return")
    params = signature.params or ()

    def returned_of(
        occurrence: int, args: Sequence[int], value: int, instructions: int
    ) -> Returned:
        return Returned(
            entry,
            occurrence,
            tuple(decode(p, b) for p, b in zip(params, args, strict=True)),
            None if signature.void else decode(signature.returns, value),
            instructions,
        )

    return entry, [p.kind for p in params], signature.returns.kind, returned_of


def copy_stub(machine: Machine, *, returns_dest: bool) -> InterceptHandler:
    def copy(call: Call) -> Return:
        dest, src, n = machine._words(call, 3)
        if n:
            machine.mem.write(dest, machine.mem.read(src, n))
        return call.returns(dest if returns_dest else None)

    return copy


def set_stub(machine: Machine, *, order: str, returns_dest: bool) -> InterceptHandler:
    def fill(call: Call) -> Return:
        words = dict(zip(order, machine._words(call, len(order)), strict=True))
        if words["n"]:
            machine.mem.write(words["d"], bytes([words.get("c", 0) & 0xFF]) * words["n"])
        return call.returns(words["d"] if returns_dest else None)

    return fill


def strlen_stub(machine: Machine, call: Call) -> Return:
    (text,) = machine._words(call, 1)
    length = 0
    while True:
        block = machine.mem.read(text + length, 64 - (text + length) % 64)
        end = block.find(b"\0")
        if end >= 0:
            return call.returns(length + end)
        length += len(block)


async def probe_hit(machine: Machine, hook: _core.ProbeHook, within: VirtualDuration) -> bool:
    machine._refuse_on_host()
    ready = machine._handle.predicate_probe_hit(hook)
    if machine._handle.session_open():
        try:
            raw, ended, deadline = cast(
                tuple[_core.StopInfo, bool, int | None],
                await awaited(
                    machine._handle.wait_predicate_timed(ready, within.ps), lambda got: got
                ),
            )
        except BaseException as error:
            machine._surfaced = error
            raise
        stop = stop_of(raw)
        machine._surfaced = stop
        if ended:
            raise stop_error(
                stop, "the free run, while waiting for a probe hit", error_prefix(machine)
            )
        first = hook.first_ps()
        return first is not None and deadline is not None and first < deadline
    try:
        start = machine.time
    except MachineRunning as error:
        where = (
            "is a member of a running linked system"
            if machine._linked() is not None
            else "runs a goal outside a free run"
        )
        raise MachineBusy(f"{machine._name} {where}: await a probe hit outside it") from error
    stop = await machine.wait_for(Native(ready), for_=within)
    first = hook.first_ps()
    missed = stop.reason is StopReason.USER and first is None
    if missed or stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
        raise stop_error(stop, "the run waiting for a probe hit", error_prefix(machine))
    return first is not None and first < start.ps + within.ps
