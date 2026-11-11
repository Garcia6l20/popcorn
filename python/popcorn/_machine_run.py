# pyright: reportPrivateUsage=false
"""Run control bodies behind the `Machine` run, step, wait and free-run methods."""

from __future__ import annotations

import contextlib
import dataclasses
import functools
from collections.abc import Callable
from typing import TYPE_CHECKING, cast

from popcorn import _core, _machine_call
from popcorn._call import decode
from popcorn._enums import StopReason
from popcorn._errors import EmulationError, MachineBusy, MachineRunning
from popcorn._host import awaited
from popcorn._image import Symbol
from popcorn._machine_checks import masked_stop
from popcorn._machine_events import (
    Event,
    Native,
    Reached,
    Ready,
    Resolver,
    Returning,
    check_duration,
    check_wait_args,
    error_prefix,
    ps_of,
    stack_access,
    stop_error,
)
from popcorn._refs import Condition, Conditions, any_of, native_set, owner_of
from popcorn._stop import FrameReturn, Stop, stop_of
from popcorn._streams import Gated, Waited, deadline_ps
from popcorn._unwind import return_point
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine


async def run(
    machine: Machine,
    *,
    until: int | str | Symbol | FrameReturn | None = None,
    budget: int | None = None,
    for_: VirtualDuration | None = None,
    stop_when: Callable[[Machine], bool] | None = None,
) -> Stop:
    check_duration(for_)
    returning = (
        Resolver(functools.partial(frame_return, machine))
        if isinstance(until, FrameReturn)
        else None
    )
    target = None if until is None or isinstance(until, FrameReturn) else machine.address_of(until)
    return await goal(
        machine,
        until=target,
        budget=machine._default_budget if budget is None else budget,
        for_=for_,
        stop_when=stop_when,
        exact=None,
        run=True,
        returning=returning,
    )


async def step_return(machine: Machine, *, budget: int | None = None) -> int | float | None:
    returning = Resolver(
        functools.partial(frame_return, machine), functools.partial(check_returns, machine)
    )
    stop = await goal(
        machine,
        until=None,
        budget=machine._default_budget if budget is None else budget,
        for_=None,
        stop_when=None,
        exact=None,
        run=True,
        returning=returning,
    )
    target = returning.target
    if target is None:
        raise EmulationError(f"{error_prefix(machine)}the step out resolved no return point")
    if stop.reason is not StopReason.RETURNED:
        raise stop_error(stop, f"the step out of {target.name}", error_prefix(machine))
    if target.exception:
        return None
    signature = _machine_call.call_signature(machine, target.name, target.start)
    if signature.void:
        return None
    slot = signature.returns
    abi = _core.FloatAbi(_machine_call.abi_at(machine, target.start).value)
    return decode(slot, machine._handle.result_bits(slot.kind, abi))


async def step(machine: Machine, n: int = 1) -> Stop:
    if n <= 0:
        raise ValueError("step count must be above 0")
    return await goal(
        machine, until=None, budget=None, for_=None, stop_when=None, exact=n, deadline=None
    )


async def wait_until(
    machine: Machine,
    predicate: Callable[[Machine], bool],
    *,
    for_: VirtualDuration | None = None,
    deadline: VirtualTime | None = None,
) -> VirtualTime | None:
    machine._refuse_on_host()
    check_wait_args(for_, deadline)
    event = Ready(predicate)
    if not machine.busy and machine._handler(event.ready)():
        return machine.time
    stop = await machine.wait_for(event, for_=for_, deadline=deadline)
    if stop.reason is StopReason.USER and event.fired:
        return stop.time
    if stop.reason in (StopReason.TIME, StopReason.IDLE):
        return None
    raise stop_error(stop, "the run waiting for the predicate", error_prefix(machine))


async def wait_for(
    machine: Machine,
    event: Event,
    *,
    for_: VirtualDuration | None = None,
    deadline: VirtualTime | None = None,
) -> Stop:
    check_wait_args(for_, deadline)
    if isinstance(event, Condition | Conditions):
        return await wait_condition(machine, event, for_, deadline)
    if machine._handle.session_open():
        if isinstance(event, Reached) and not machine._on_host():
            return await reached_free(machine, machine.address_of(event.target), for_, deadline)
        return await waited(machine, event, for_, deadline)
    if isinstance(event, Reached):
        return await bounded_run(machine, until=event.target, for_=for_, deadline=deadline)
    if isinstance(event, Native):
        return await run_native(machine, event.predicate, for_, deadline)
    if isinstance(event, Ready) and event.gate is not None:
        return await run_gated(machine, event.gate, event.ready, for_, deadline)
    return await bounded_run(machine, for_=for_, deadline=deadline, stop_when=event.ready)


async def elapse(machine: Machine, duration: VirtualDuration) -> VirtualTime:
    machine._refuse_on_host()
    check_duration(duration)
    if machine._handle.session_open():
        stop = await waited(machine, None, duration)
    else:
        stop = await goal(
            machine,
            until=None,
            budget=machine._default_budget,
            for_=duration,
            stop_when=None,
            exact=None,
        )
    if stop.reason is not StopReason.TIME:
        raise stop_error(stop, "the run letting time pass", error_prefix(machine))
    return stop.time


def start(machine: Machine) -> None:
    machine._refuse_on_host()
    if machine._linked() is not None:
        raise MachineBusy(
            f"{machine._name} is a member of a linked system: use the member's running()"
        )
    machine._handle.start()
    machine._last_stop = None
    machine._surfaced = None


def halt(machine: Machine) -> None:
    system = machine._linked()
    if system is not None:
        machine._refuse_on_host()
        system._halt_member(machine)  # pyright: ignore[reportPrivateUsage]
        return
    ended = halted(machine)
    if isinstance(ended, BaseException):
        raise ended
    if ended is not None:
        raise stop_error(ended, prefix=error_prefix(machine))
    if machine._parked_ends:
        raise machine._parked_ends.pop(0)


async def wait_ready(
    machine: Machine,
    ready: _core.Predicate | Gated,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None = None,
) -> Waited:
    if isinstance(ready, Gated):
        return await wait_gated(machine, ready, for_, deadline)
    if not machine.busy:
        with contextlib.suppress(MachineRunning):
            if ready.holds():
                return Waited(machine.time, None)
    if machine._handle.session_open():
        raw, ended = cast(
            tuple[_core.StopInfo, bool],
            await awaited(
                machine._handle.wait_predicate_ended(ready, ps_of(for_), deadline_ps(deadline)),
                lambda got: got,
            ),
        )
        stop = stop_of(raw)
        if ended:
            raise stop_error(
                stop, f"the free run, while waiting for {ready.what()}", error_prefix(machine)
            )
        return Waited(stop.time, stop.resets)
    stop = await machine.wait_for(Native(ready), for_=for_, deadline=deadline)
    if stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
        raise stop_error(stop, f"the run waiting for {ready.what()}", error_prefix(machine))
    return Waited(stop.time, stop.resets)


def park(machine: Machine) -> BaseException | None:
    machine._refuse_on_host()
    try:
        raw = machine._handle.park()
    except Exception as kept:
        machine._parked_ends.append(kept)
        return kept
    ended = ended_of(machine, raw)
    surfaced, machine._surfaced = machine._surfaced, None
    if ended is None or ended is surfaced:
        return None
    if isinstance(ended, Stop):
        ended = stop_error(ended, prefix=error_prefix(machine))
    machine._parked_ends.append(ended)
    return ended


def check_returns(machine: Machine, target: Returning) -> None:
    if target.exception:
        return
    unsupported = _machine_call.call_signature(
        machine, target.name, target.start
    ).returns_unsupported
    if unsupported is not None:
        raise NotImplementedError(unsupported)


def frame_return(machine: Machine) -> Returning:
    regs = machine.reg
    pc, lr, sp = int(regs.pc), int(regs.lr), int(regs.sp)
    read32, functions = stack_access(machine)
    function = functions.at(pc)
    name = f"{pc:#x}" if function is None else function.name
    point = return_point(read32, functions, pc, lr, sp)
    if point is None:
        raise EmulationError(
            f"{error_prefix(machine)}{name}: cannot find where the function at {pc:#x} returns"
        )
    start = pc if function is None else function.start
    return Returning(point.ret, point.sp, name, start, pc)


async def bounded_run(
    machine: Machine,
    *,
    until: int | str | Symbol | None = None,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None,
    stop_when: Callable[[Machine], bool] | None = None,
) -> Stop:
    return await goal(
        machine,
        until=None if until is None else machine.address_of(until),
        budget=machine._default_budget,
        for_=for_,
        deadline=deadline,
        stop_when=stop_when,
        exact=None,
    )


async def goal(
    machine: Machine,
    *,
    until: int | None,
    budget: int | None,
    for_: VirtualDuration | None,
    stop_when: Callable[[Machine], bool] | None,
    exact: int | None,
    deadline: VirtualTime | None = None,
    run: bool = False,
    returning: Resolver | None = None,
) -> Stop:
    machine._refuse_in_step()
    machine._last_stop = None
    system = machine._linked()
    if system is not None:
        stop = await system._goal(  # pyright: ignore[reportPrivateUsage]
            machine,
            until=until,
            budget=budget,
            exact=exact,
            for_=for_,
            deadline=deadline,
            stop_when=None if stop_when is None else machine._handler(stop_when),
            run=run,
            returning=returning,
        )
        if returning is not None:
            stop = returning.named(stop)
        machine._last_stop = stop
        return stop
    until_return: tuple[int, int] | None = None
    if returning is not None:
        if machine.busy:
            raise MachineBusy("the machine already runs a goal or a free run")
        machine._handle.apply_pending_reset()
        until_return = returning()
    stop = await awaited(
        machine._handle.run_goal(
            until=until,
            budget=budget,
            exact=exact,
            for_ps=ps_of(for_),
            deadline_ps=deadline_ps(deadline),
            stop_when=None if stop_when is None else machine._handler(stop_when),
            until_return=until_return,
        ),
        lambda raw: stop_of(cast(_core.StopInfo, raw)),
    )
    if returning is not None:
        stop = returning.named(stop)
    machine._last_stop = stop
    return stop


async def wait_gated(
    machine: Machine,
    gated: Gated,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None,
) -> Waited:
    ready = gated.ready
    event = Ready(lambda _: ready(), gate=gated.gate)
    if not machine.busy and machine._handler(event.ready)():
        return Waited(machine.time, None)
    stop = await machine.wait_for(event, for_=for_, deadline=deadline)
    if stop.reason is StopReason.USER and not event.fired:
        raise stop_error(
            stop, "the free run, while waiting for a Python predicate", error_prefix(machine)
        )
    if stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
        raise stop_error(stop, "the run waiting for a Python predicate", error_prefix(machine))
    return Waited(stop.time, stop.resets)


async def wait_condition(
    machine: Machine,
    event: Condition | Conditions,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None = None,
) -> Stop:
    if owner_of(event) is not machine._handle:
        raise ValueError(f"{event!r} reads another machine")
    conditions = event if isinstance(event, Conditions) else any_of(event)
    members = conditions.members
    if not machine._handle.session_open():
        stopped: list[tuple[Condition, ...]] = []

        def stop_when(machine: Machine) -> bool:
            held = event.ready(machine)
            if held:
                stopped.append(tuple(member for member in members if member.ready(machine)))
            return held

        stop = await bounded_run(machine, for_=for_, deadline=deadline, stop_when=stop_when)
        held = stop.reason is StopReason.USER and bool(stopped)
        fired = stopped[-1] if held else ()
        stop = dataclasses.replace(stop, fired=fired)
        machine._last_stop = stop
        return stop
    try:
        stop, mask = await awaited(
            machine._handle.wait_when(native_set(conditions), ps_of(for_), deadline_ps(deadline)),
            masked_stop,
        )
    except BaseException as error:
        machine._surfaced = error
        raise
    fired = tuple(member for index, member in enumerate(members) if mask >> index & 1)
    stop = dataclasses.replace(stop, fired=fired)
    machine._surfaced = stop
    return stop


def wait_future(
    machine: Machine, event: Event | None, for_ps: int | None, deadline_ps: int | None
) -> _core.HostFuture:
    if event is None:
        return machine._handle.wait(None, for_ps, deadline_ps)
    if isinstance(event, Native):
        return machine._handle.wait_predicate(event.predicate, for_ps, deadline_ps)
    if isinstance(event, Ready) and event.gate is not None:
        return machine._handle.wait_gated(
            event.gate, machine._handler(event.ready), for_ps, deadline_ps
        )
    return machine._handle.wait(machine._handler(event.ready), for_ps, deadline_ps)


async def reached_free(
    machine: Machine, address: int, for_: VirtualDuration | None, deadline: VirtualTime | None
) -> Stop:
    hook = machine._handle.add_probe(address, None)
    try:
        return await waited(
            machine, Native(machine._handle.predicate_probe_hit(hook)), for_, deadline
        )
    finally:
        machine._handle.remove_probe(hook)


async def waited(
    machine: Machine,
    event: Event | None,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None = None,
) -> Stop:
    try:
        stop = await awaited(
            wait_future(machine, event, ps_of(for_), deadline_ps(deadline)),
            lambda raw: stop_of(cast(_core.StopInfo, raw)),
        )
    except BaseException as error:
        machine._surfaced = error
        raise
    machine._surfaced = stop
    return stop


async def run_native(
    machine: Machine,
    ready: _core.Predicate,
    for_: VirtualDuration | None,
    deadline: VirtualTime | None = None,
) -> Stop:
    machine._refuse_in_step()
    machine._last_stop = None
    system = machine._linked()
    if system is not None:
        stop = await system._goal(  # pyright: ignore[reportPrivateUsage]
            machine, budget=machine._default_budget, for_=for_, deadline=deadline, gate=ready
        )
        machine._last_stop = stop
        return stop
    stop = await awaited(
        machine._handle.run_predicate(
            ready, machine._default_budget, ps_of(for_), deadline_ps(deadline)
        ),
        lambda raw: stop_of(cast(_core.StopInfo, raw)),
    )
    machine._last_stop = stop
    return stop


async def run_gated(
    machine: Machine,
    gate: _core.Predicate,
    ready: Callable[[Machine], bool],
    for_: VirtualDuration | None,
    deadline: VirtualTime | None,
) -> Stop:
    machine._last_stop = None
    system = machine._linked()
    if system is not None:
        stop = await system._goal(  # pyright: ignore[reportPrivateUsage]
            machine,
            budget=machine._default_budget,
            for_=for_,
            deadline=deadline,
            stop_when=machine._handler(ready),
            gate=gate,
        )
        machine._last_stop = stop
        return stop
    stop = await awaited(
        machine._handle.run_gated(
            gate,
            machine._handler(ready),
            machine._default_budget,
            ps_of(for_),
            deadline_ps(deadline),
        ),
        lambda raw: stop_of(cast(_core.StopInfo, raw)),
    )
    machine._last_stop = stop
    return stop


def halted(machine: Machine) -> Stop | BaseException | None:
    machine._refuse_on_host()
    ended = ended_of(machine, machine._handle.halt())
    machine._surfaced = None
    return ended


def ended_of(machine: Machine, ended: object) -> Stop | BaseException | None:
    if isinstance(ended, BaseException):
        return ended
    if ended is None:
        return None
    stop = stop_of(cast(_core.StopInfo, ended))
    machine._last_stop = stop
    return stop
