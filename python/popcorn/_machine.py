"""The emulated machine and its asyncio slice driver."""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import os
from collections.abc import AsyncGenerator, Callable, Generator, Iterable
from dataclasses import dataclass
from typing import Protocol, runtime_checkable

from popcorn import _core
from popcorn._description import MachineDescription, load_description
from popcorn._enums import ResetKind, StopReason
from popcorn._errors import (
    BudgetExceeded,
    EmulationError,
    FaultError,
    MachineBusy,
    MachineRunning,
    SymbolError,
)
from popcorn._image import Image, Location, Symbol, location_of
from popcorn._stop import Stop, stop_of
from popcorn.units import VirtualDuration, VirtualTime

_HANDLER_OF: contextvars.ContextVar[Machine | None] = contextvars.ContextVar(
    "popcorn_handler_of", default=None
)


def _check_duration(for_: VirtualDuration | None) -> None:
    if for_ is not None and for_.ps < 0:
        raise ValueError(f"for_ must not be negative, got {for_}")


def _stop_error(stopped: Stop) -> EmulationError:
    message = f"the background run stopped: {stopped}"
    if stopped.reason is StopReason.FAULT and stopped.fault is not None:
        fault = stopped.fault
        return FaultError(
            message,
            kind=fault.kind,
            address=fault.address,
            cfsr=fault.cfsr,
            hfsr=fault.hfsr,
            pc=stopped.pc,
            symbol=stopped.location.symbol,
            stop=stopped,
        )
    if stopped.reason is StopReason.BUDGET:
        return BudgetExceeded(
            message,
            budget=stopped.instructions,
            pc=stopped.pc,
            symbol=stopped.location.symbol,
            stop=stopped,
        )
    return EmulationError(
        message,
        reason=stopped.reason,
        pc=stopped.pc,
        symbol=stopped.location.symbol,
        stop=stopped,
    )


@runtime_checkable
class Event(Protocol):
    """Something `Machine.wait_for` waits for.

    `ready` is sync and runs at event boundaries and slice ends, never per instruction. It may
    read the machine, which is stopped at an instruction boundary while it runs.
    """

    def ready(self, machine: Machine) -> bool:
        """Whether the event happened."""
        ...


@dataclass(frozen=True, slots=True)
class Reached:
    """The core reaching an address or symbol, before executing it.

    `wait_for` stops exactly there, outside `running()`. Inside `running()` it is checked at slice
    boundaries like any other event.
    """

    target: int | str | Symbol

    def ready(self, machine: Machine) -> bool:
        """Whether pc is at the target."""
        return machine.pc == machine.address_of(self.target)


@dataclass(slots=True)
class _Waiter:
    event: Event
    deadline_ps: int | None
    future: asyncio.Future[Stop]


class Machine:
    """One emulated MCU: a description, the images loaded into it and its run state.

    Built powered off: nothing runs until `reset()`. Emulation advances in slices on the event
    loop thread, each bounded by `slice_instructions` and `slice_wall_budget`, yielding to the
    loop between slices. Sync access to the machine while a run is active raises
    MachineRunning, except from a handler of that run.
    """

    __slots__ = (
        "_background",
        "_busy",
        "_config_digest",
        "_description",
        "_handle",
        "_images",
        "_slice_instructions",
        "_slice_wall_ns",
        "_waiters",
    )

    def __init__(
        self,
        description: MachineDescription | str | os.PathLike[str],
        *,
        image: Image | Iterable[Image] = (),
        core_hz: int = 0,
        cpi: float = 1.0,
        trace_capacity: int = 4096,
        spin_threshold: int = 0,
        strict_peripherals: bool = False,
        vector_table: int | None = None,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
        config_digest: str = "",
    ) -> None:
        """Build a machine from a description or the path of its JSON, then load `image`.

        Args:
            description: The description, or the path of its JSON file.
            image: Image or images to load, in order.
            core_hz: Core clock at power-on in Hz, 0 for the description's reset clock.
            cpi: Core cycles per instruction, above 0.
            trace_capacity: Records the always-on trace ring keeps, above 0.
            spin_threshold: Iterations of a loop making no progress before a SPIN stop, 0 off.
            strict_peripherals: Whether an unmodelled register access raises ModelError.
            vector_table: Address of the vector table reset reads, None for the default.
            slice_instructions: Instructions after which a slice yields to the event loop.
            slice_wall_budget: Wall seconds after which a slice yields to the event loop.
            config_digest: Digest of the configuration, such as a project file's, that snapshots
                record.

        Raises:
            ConfigError: No arch module implements the core, or an option is out of range.
            ImageError: An image does not fit the memory map.
        """
        if slice_instructions <= 0 or not slice_wall_budget > 0:
            raise ValueError("slice_instructions and slice_wall_budget must be above 0")
        if not isinstance(description, MachineDescription):
            description = load_description(description)
        config = _core.MachineConfig()
        config.core_hz = core_hz
        config.cpi = cpi
        config.trace_capacity = trace_capacity
        config.spin_threshold = spin_threshold
        config.strict_peripherals = strict_peripherals
        config.vector_table = vector_table
        config.config_digest = config_digest
        self._description = description
        self._config_digest = config_digest
        self._handle = _core.MachineHandle(description, config)
        self._images: list[Image] = []
        self._slice_instructions = slice_instructions
        self._slice_wall_ns = max(1, round(slice_wall_budget * 1e9))
        self._busy = False
        self._background = False
        self._waiters: list[_Waiter] = []
        for one in (image,) if isinstance(image, Image) else image:
            self.load(one)

    @property
    def description(self) -> MachineDescription:
        """Description the machine was built from."""
        return self._description

    @property
    def config_digest(self) -> str:
        """Digest of the configuration the machine was built from, empty when none."""
        return self._config_digest

    @property
    def images(self) -> tuple[Image, ...]:
        """Images loaded so far, in load order."""
        return tuple(self._images)

    @property
    def busy(self) -> bool:
        """Whether a run, step, wait or `running()` is active."""
        return self._busy

    @property
    def instructions(self) -> int:
        """Instructions executed since reset."""
        self._require_stopped()
        return self._handle.instructions()

    @property
    def time(self) -> VirtualTime:
        """Virtual time since reset."""
        self._require_stopped()
        return VirtualTime(self._handle.time_ps())

    @property
    def pc(self) -> int:
        """Address of the next instruction."""
        self._require_stopped()
        return self._handle.pc()

    def load(self, image: Image) -> None:
        """Load one more image over memory, such as an application after a bootloader.

        Raises:
            ImageError: A segment falls outside the description's memory regions.
            MachineRunning: A run is active.
        """
        self._require_stopped()
        self._handle.load(image.native)
        self._images.append(image)

    def reset(self, kind: ResetKind = ResetKind.POWER_ON) -> None:
        """Reset: clock to 0, core from its vector table, peripherals to reset values.

        Memory is kept.

        Raises:
            MachineRunning: A run is active, handlers of it included.
        """
        self._require_stopped()
        self._handle.reset(_core.ResetKind(kind.value))

    def _write_memory(self, address: int, data: bytes) -> None:
        """Write bytes into guest memory as the debugger, flash included, for `Project.machine`
        until `Machine.mem` exists.

        Raises:
            ConfigError: A byte is not in writable memory or the range leaves the 32-bit address
                space, keyed by the address.
            MachineRunning: A run is active.
        """
        self._require_stopped()
        self._handle.write_memory(address, data)

    def locate(self, address: int) -> Location:
        """Symbol, offset, file and line of an address, the rule stops use."""
        return location_of(self._handle.locate(address))

    def address_of(self, target: int | str | Symbol) -> int:
        """Address of a target: an address as is, a symbol, or a symbol name.

        A name is looked up in the loaded images, the last loaded first.

        Raises:
            SymbolError: No loaded image has the name.
        """
        if isinstance(target, int):
            return target
        if isinstance(target, Symbol):
            return target.value
        missing: SymbolError | None = None
        for image in reversed(self._images):
            try:
                return image.lookup(target).value
            except SymbolError as error:
                missing = missing or error
        raise missing or SymbolError(f"no image loaded to look up {target}", name=target)

    async def run(
        self,
        *,
        until: int | str | Symbol | None = None,
        budget: int | None = None,
        for_: VirtualDuration | None = None,
        stop_when: Callable[[Machine], bool] | None = None,
    ) -> Stop:
        """Run until a stop, one slice at a time, yielding to the event loop between slices.

        Args:
            until: Stop with ADDRESS before executing this address or symbol.
            budget: Stop with BUDGET at the first block boundary past this many instructions.
            for_: Stop with TIME after this much virtual time. An idle core, WFI with nothing
                due, sleeps to the deadline and stops with TIME, not IDLE.
            stop_when: Sync predicate evaluated at event boundaries and slice ends, never per
                instruction. Stops with USER when it returns True. An exception it raises
                reaches the caller with its traceback.

        Raises:
            MachineBusy: Another run, step, wait or `running()` is active.
        """
        _check_duration(for_)
        target = None if until is None else self.address_of(until)
        with self._exclusive():
            return await self._drive(
                until=target,
                budget=budget,
                for_=for_,
                stop_when=stop_when,
                exact=None,
            )

    async def step(self, n: int = 1) -> Stop:
        """Execute exactly `n` instructions, a BUDGET stop, or an earlier stop.

        Raises:
            MachineBusy: Another run, step, wait or `running()` is active.
        """
        if n <= 0:
            raise ValueError("step count must be above 0")
        with self._exclusive():
            return await self._drive(until=None, budget=None, for_=None, stop_when=None, exact=n)

    async def wait_for(self, event: Event, *, for_: VirtualDuration | None = None) -> Stop:
        """Run until `event` is ready, a USER stop, or until `for_` elapses, a TIME stop.

        A `Reached` event stops exactly before its target, an ADDRESS stop. Other events stop at
        the first event boundary or slice end after they became ready. Inside `running()`, the
        background run is not driven twice: this awaits the driver, which checks the event at
        slice boundaries.

        Raises:
            MachineBusy: A run, step or wait is active outside `running()`.
        """
        _check_duration(for_)
        if self._background:
            return await self._await_driver(event, for_)
        if isinstance(event, Reached):
            return await self.run(until=event.target, for_=for_)
        return await self.run(for_=for_, stop_when=event.ready)

    @contextlib.asynccontextmanager
    async def running(self) -> AsyncGenerator[None]:
        """Keep the machine running in a background task while the body awaits other things.

        An IDLE stop waits one wall budget and runs again, the firmware waiting for input. Any
        other stop ends the background run, and leaving the body then raises the error of that
        stop, carrying it: FaultError for FAULT, BudgetExceeded for BUDGET, EmulationError
        otherwise. When the body itself raises, its exception propagates with a note naming the
        background stop. Leaving the body cancels the run at a slice boundary.

        Raises:
            MachineBusy: A run, step or wait is active.
            EmulationError: The background run stopped for a reason other than IDLE.
        """
        with self._exclusive():
            self._background = True
            task = asyncio.create_task(self._drive_background())
            try:
                yield
            except BaseException as error:
                stopped = await self._end_background(task)
                if stopped is not None:
                    error.add_note(f"background run stopped: {stopped}")
                raise
            stopped = await self._end_background(task)
        if stopped is not None:
            raise _stop_error(stopped)

    async def _end_background(self, task: asyncio.Task[Stop | None]) -> Stop | None:
        task.cancel()
        stopped: Stop | None = None
        with contextlib.suppress(asyncio.CancelledError):
            stopped = await task
        self._background = False
        self._release_waiters(None)
        return stopped

    @contextlib.contextmanager
    def _exclusive(self) -> Generator[None]:
        if self._busy:
            raise MachineBusy("a run, step, wait or running() is already active on this machine")
        self._busy = True
        try:
            yield
        finally:
            self._busy = False

    def _require_stopped(self) -> None:
        if self._busy and _HANDLER_OF.get() is not self:
            raise MachineRunning("the machine is running, stop or await it first")

    def _handler(self, predicate: Callable[[Machine], bool]) -> Callable[[], bool]:
        def call() -> bool:
            token = _HANDLER_OF.set(self)
            try:
                return bool(predicate(self))
            finally:
                _HANDLER_OF.reset(token)

        return call

    def _slice_args(self) -> _core.SliceArgs:
        args = _core.SliceArgs()
        args.max_instructions = self._slice_instructions
        args.wall_budget_ns = self._slice_wall_ns
        return args

    async def _drive(
        self,
        *,
        until: int | None,
        budget: int | None,
        for_: VirtualDuration | None,
        stop_when: Callable[[Machine], bool] | None,
        exact: int | None,
    ) -> Stop:
        args = self._slice_args()
        args.until = until
        if for_ is not None:
            args.until_time_ps = self._handle.time_ps() + for_.ps
        predicate = None if stop_when is None else self._handler(stop_when)
        remaining = budget
        left = exact
        while True:
            if remaining is not None:
                args.budget_left = remaining
            if left is not None:
                args.exact_instructions = left
            outcome = self._handle.run_slice(args, predicate)
            if outcome.stopped is not None:
                return stop_of(outcome.stopped)
            if remaining is not None:
                remaining = max(remaining - outcome.executed, 0)
            if left is not None:
                left = max(left - outcome.executed, 1)
            await asyncio.sleep(0)

    async def _drive_background(self) -> Stop | None:
        args = self._slice_args()
        while True:
            deadlines = [
                w.deadline_ps
                for w in self._waiters
                if w.deadline_ps is not None and not w.future.done()
            ]
            args.until_time_ps = min(deadlines, default=None)
            outcome = self._handle.run_slice(args, None)
            stopped = None if outcome.stopped is None else stop_of(outcome.stopped)
            self._check_waiters()
            if stopped is None or stopped.reason is StopReason.TIME:
                await asyncio.sleep(0)
            elif stopped.reason is StopReason.IDLE:
                await asyncio.sleep(self._slice_wall_ns / 1e9)
            else:
                self._release_waiters(stopped)
                return stopped

    async def _await_driver(self, event: Event, for_: VirtualDuration | None) -> Stop:
        deadline = None if for_ is None else self._handle.time_ps() + for_.ps
        future: asyncio.Future[Stop] = asyncio.get_running_loop().create_future()
        self._waiters.append(_Waiter(event, deadline, future))
        return await future

    def _stop_here(self, reason: StopReason) -> Stop:
        pc = self._handle.pc()
        return Stop(
            reason=reason,
            pc=pc,
            location=location_of(self._handle.locate(pc)),
            instructions=self._handle.instructions(),
            time=VirtualTime(self._handle.time_ps()),
        )

    def _check_waiters(self) -> None:
        now = self._handle.time_ps()
        for waiter in list(self._waiters):
            if waiter.future.done():
                self._waiters.remove(waiter)
                continue
            try:
                if self._handler(waiter.event.ready)():
                    waiter.future.set_result(self._stop_here(StopReason.USER))
                elif waiter.deadline_ps is not None and now >= waiter.deadline_ps:
                    waiter.future.set_result(self._stop_here(StopReason.TIME))
            except Exception as error:
                waiter.future.set_exception(error)
            if waiter.future.done():
                self._waiters.remove(waiter)

    def _release_waiters(self, stopped: Stop | None) -> None:
        waiters, self._waiters = self._waiters, []
        for waiter in waiters:
            if waiter.future.done():
                continue
            if stopped is None:
                waiter.future.cancel()
            else:
                waiter.future.set_result(stopped)

    def __repr__(self) -> str:
        return f"Machine({self._description.part}, images={len(self._images)})"
