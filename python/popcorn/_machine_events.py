from __future__ import annotations

import contextvars
import weakref
from collections.abc import Callable, Sequence
from dataclasses import dataclass, replace
from typing import (
    TYPE_CHECKING,
    Protocol,
    runtime_checkable,
)

from popcorn import _core
from popcorn._call import (
    F32,
    I64,
    Signature,
    Slot,
    decode,
    encode,
    slot_for,
)
from popcorn._enums import FloatAbi, ResetKind, StopReason, ValueKind
from popcorn._errors import (
    BudgetExceeded,
    ConfigError,
    EmulationError,
    FaultError,
)
from popcorn._hooks import (
    Return,
    Returned,
    WatchAction,
    WatchHit,
)
from popcorn._image import Image, Symbol
from popcorn._stop import Stop
from popcorn._streams import (
    check_wait,
)
from popcorn._unwind import Functions, Read32
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._machine import Machine


class Call:
    """A call arriving at an intercepted function, stopped at its first instruction.

    Valid only while the handler runs.
    """

    __slots__ = ("_signature", "_view", "machine")

    def __init__(self, machine: Machine, view: _core.InterceptCall, signature: Signature) -> None:
        """Wrap a native call. Handlers receive it built."""
        self.machine = machine
        self._view = view
        self._signature = signature

    @property
    def function(self) -> int:
        """Address of the intercepted function."""
        return self._view.function()

    @property
    def occurrence(self) -> int:
        """Calls of this intercept before this one, 0 for the first."""
        return self._view.occurrence()

    @property
    def lr(self) -> int:
        """Address the function returns to."""
        return self._view.return_address()

    @property
    def sp(self) -> int:
        """Stack pointer at entry."""
        return self.machine.reg.sp

    def args(
        self, kinds: Sequence[ValueKind], abi: FloatAbi | None = None
    ) -> tuple[int | float, ...]:
        """Arguments of the given kinds, integers signed, floats decoded, under the image's
        calling convention or `abi`."""
        native = [_core.ValueKind(k.value) for k in kinds]
        bits = self._view.args(native, None if abi is None else _core.FloatAbi(abi.value))
        return tuple(decode(slot_for(k), b) for k, b in zip(kinds, bits, strict=True))

    def args_typed(self, abi: FloatAbi | None = None) -> tuple[int | float, ...]:
        """Arguments read with the function's DWARF signature, each at its C type.

        Args:
            abi: Calling convention to read floats with, the image's by default.

        Raises:
            TypeError: The function has no DWARF signature.
        """
        params = self._signature.params
        if params is None:
            reason = self._signature.unsupported or f"{self._signature.name} has no DWARF signature"
            raise TypeError(f"{reason}, use args(kinds)")
        native = [p.kind for p in params]
        bits = self._view.args(native, None if abi is None else _core.FloatAbi(abi.value))
        return tuple(decode(p, b) for p, b in zip(params, bits, strict=True))

    def arg(self, index: int) -> int:
        """Argument `index` as an integer: from DWARF when known, else the index-th 32-bit
        integer argument."""
        if self._signature.params is not None:
            value = self.args_typed()[index]
            if not isinstance(value, int):
                raise TypeError(f"argument {index} of {self._signature.name} is a float")
            return value
        value = self.args([ValueKind.I32] * (index + 1))[index]
        assert isinstance(value, int)
        return value

    def arg_f(self, index: int) -> float:
        """Argument `index` as a float, from the DWARF signature."""
        value = self.args_typed()[index]
        if not isinstance(value, float):
            raise TypeError(f"argument {index} of {self._signature.name} is an integer")
        return value

    def proceed(self) -> Return:
        """Run the function as if no intercept were there."""
        return Return(_core.InterceptResult.proceed())

    def returns(
        self, value: int | float | I64 | F32 | None = None, abi: FloatAbi | None = None
    ) -> Return:
        """Skip the function and return `value` to the caller, of the DWARF return type when
        known, under the image's calling convention or `abi`. None for a void function. From an
        exception handler, the return is an exception return, as BX LR would be."""
        if value is None:
            return Return(_core.InterceptResult.returns(None))
        slot = self._result_slot(value)
        number = value.value if isinstance(value, I64 | F32) else value
        arg = _core.CallArg()
        arg.kind = slot.kind
        arg.bits = encode(slot.kind, number, f"the return value of {self._signature.name}")
        native_abi = None if abi is None else _core.FloatAbi(abi.value)
        return Return(_core.InterceptResult.returns(arg, native_abi), number)

    def stop(self, reason: StopReason = StopReason.USER) -> Return:
        """Stop the run at the function, before its body.

        A parked `run` returns the stop. In a free run the stop ends it: a pending `wait_for`
        returns it, and `halt()` or leaving `running()` raises EmulationError carrying it. A test
        that cuts power at a function stops there, then calls `power_cycle()`.

        Raises:
            ValueError: `reason` is FAULT, which only the core raises.
        """
        if reason is StopReason.FAULT:
            raise ValueError("an intercept cannot stop with FAULT")
        return Return(_core.InterceptResult.stop(_core.StopReason(reason.value)))

    def _result_slot(self, value: int | float | I64 | F32) -> Slot:
        if isinstance(value, I64):
            return slot_for(ValueKind.I64)
        if isinstance(value, F32):
            return slot_for(ValueKind.F32)
        if self._signature.params is not None:
            return self._signature.returns
        return slot_for(ValueKind.F64 if isinstance(value, float) else ValueKind.I32)


type InterceptHandler = Callable[[Call], Return]
"""An intercept handler: the call in, what to do out."""
type ReturnHandler = Callable[[Returned], None]
"""An on_return handler."""
type WatchHandler = Callable[[WatchHit], WatchAction | None]
"""A watch handler: STOP, RESUME, or None for RESUME."""


DEFAULT_ARENA_BASE = 0x6F00_0000
"""Default first address of the call arena, an FMC address STM32 parts leave unmapped."""
DEFAULT_ARENA_SIZE = 1 << 20
"""Default size of the call arena."""

RUNNING = "the machine is running: await m.read() or m.write(), or halt() it first"
FIRST_IRQ = 16
SYSTEM_EXCEPTIONS = {2: "NMI", 14: "PendSV", 15: "SysTick"}


class ContextCell:
    """The context the Python models and bus devices of a machine run in, a copy of the one
    current at `Machine()`, and the exceptions that ended free runs, by token, each while native
    code holds its end. Native code holds the cell weakly, so a cycle through it is collectable."""

    __slots__ = ("__weakref__", "context", "first", "held")

    def __init__(self, context: contextvars.Context) -> None:
        self.context = context
        self.first = context.copy()
        self.held: dict[int, BaseException] = {}

    def restart(self) -> None:
        """Run the next callbacks in a new copy of the context current at `Machine()`."""
        self.context = self.first.copy()

    def run(self, callback: Callable[[], object]) -> None:
        """Call `callback` in the context, so a callback native code enters inside it nests."""
        _core.run_in_cell(weakref.ref(self), callback)


def check_duration(for_: VirtualDuration | None) -> None:
    if for_ is not None and for_.ps < 0:
        raise ValueError(f"for_ must not be negative, got {for_}")


def check_wait_args(for_: VirtualDuration | None, deadline: VirtualTime | None) -> None:
    check_wait(for_, deadline)
    check_duration(for_)


def ps_of(value: VirtualDuration | None) -> int | None:
    return None if value is None else value.ps


def check_machine_name(name: str | None) -> None:
    """Refuse a machine name `Machine(name=)` would refuse, None passing.

    Raises:
        ValueError: `name` is empty or holds whitespace or a dot.
    """
    if name is None:
        return
    if not name or "." in name or any(c.isspace() for c in name):
        raise ValueError(f"name {name!r} must be non-empty, without whitespace or a dot")


def error_prefix(machine: Machine) -> str:
    """What starts the text of an error a machine raises: `<name>: ` when the machine has a
    name other than its part, empty otherwise."""
    return "" if machine.name == machine.description.part else f"{machine.name}: "


def stack_access(machine: Machine) -> tuple[Read32, Functions]:
    """A word reader for the unwinder, None outside the regions of `machine`'s description or
    on an access error, and the function symbols of its loaded images, every image's own
    where two define one name."""
    regions = [(r.start, r.start + r.size) for r in machine.description.regions]

    def read32(address: int) -> int | None:
        if not any(start <= address and address + 4 <= end for start, end in regions):
            return None
        try:
            return machine.mem.u32[address]
        except EmulationError:
            return None

    functions = Functions(
        (s.name, s.value, s.size) for image in machine.images for s in image.symbols if s.function
    )
    return read32, functions


@dataclass(frozen=True, slots=True)
class Returning:
    """Where the function running at `pc` returns, a frame-return goal's target."""

    ret: int
    sp: int
    name: str
    start: int
    pc: int

    @property
    def exception(self) -> bool:
        """Whether the function is an exception handler, returning through EXC_RETURN."""
        return self.ret >> 24 == 0xFF


class Resolver:
    """Resolves a frame-return target once, when the goal is posted, and keeps it."""

    __slots__ = ("_check", "_resolve", "target")

    def __init__(
        self,
        resolve: Callable[[], Returning],
        check: Callable[[Returning], None] | None = None,
    ) -> None:
        self._resolve = resolve
        self._check = check
        self.target: Returning | None = None

    def __call__(self) -> tuple[int, int]:
        target = self._resolve()
        if self._check is not None:
            self._check(target)
        self.target = target
        return target.ret, target.sp

    def named(self, stop: Stop) -> Stop:
        """`stop` with the detail `<function> returned` when it is this goal's RETURNED."""
        if self.target is None or stop.reason is not StopReason.RETURNED:
            return stop
        return replace(stop, detail=f"{self.target.name} returned")


def stop_error(
    stopped: Stop, during: str = "the background run", prefix: str = ""
) -> EmulationError:
    message = f"{prefix}{during} stopped with {stopped.reason.name}: {stopped}"
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

    `ready` is sync and runs on the machine's thread, never per instruction: at slice ends
    inside a free run, `start()` or `running()`, after every event and at instruction-count
    slice ends otherwise.
    It may read the machine, which is stopped at an instruction boundary there.
    """

    def ready(self, machine: Machine) -> bool:
        """Whether the event happened.

        Args:
            machine: The machine to test.
        """
        ...


@dataclass(frozen=True, slots=True)
class Reached:
    """The core reaching an address or symbol, before executing it.

    `wait_for` stops exactly there, outside `running()`. Inside `running()` and `start()` it is
    exact too: a probe at the target, `Machine.probe`, sees the arrival at the instruction, also
    one executed once in the middle of a slice, and the stop is the end of that slice, a USER
    stop. There a target equal to the current pc counts at its next execution, where a parked
    wait continues as `run(until=)` does.
    """

    target: int | str | Symbol

    def ready(self, machine: Machine) -> bool:
        """Whether pc is at the target.

        Args:
            machine: The machine to test.
        """
        return machine.pc == machine.address_of(self.target)


@dataclass(slots=True)
class Ready:
    predicate: Callable[[Machine], bool]
    fired: bool = False
    gate: _core.Predicate | None = None

    def ready(self, machine: Machine) -> bool:
        held = bool(self.predicate(machine))
        self.fired = self.fired or held
        return held


@dataclass(frozen=True, slots=True)
class Native:
    predicate: _core.Predicate

    def ready(self, machine: Machine) -> bool:
        return self.predicate.holds()


class Linked(Protocol):
    """What a member hands its runs to: its `System`."""

    async def _goal(
        self,
        machine: Machine,
        *,
        until: int | None = None,
        budget: int | None = None,
        exact: int | None = None,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
        stop_when: Callable[[], bool] | None = None,
        gate: _core.Predicate | None = None,
        run: bool = False,
        returning: Callable[[], tuple[int, int]] | None = None,
    ) -> Stop: ...

    def _halt_member(self, machine: Machine) -> None: ...

    async def _open_goal(self, machine: Machine) -> None: ...

    async def _close_open(self, machine: Machine) -> Stop | None: ...

    def reset(self, member: str, kind: ResetKind = ResetKind.POWER_ON) -> None: ...


def check_image_names(images: Sequence[Image]) -> None:
    seen: set[str] = set()
    for index, image in enumerate(images):
        if image.name is None:
            continue
        if image.name in seen:
            raise ConfigError(
                f"two images are named {image.name}",
                file="images",
                key=f"images[{index}].name",
                expected="a name no other image has",
            )
        seen.add(image.name)


def probe_state(state: object, registers: Sequence[str]) -> bool | None:
    if state is None:
        return None
    if state not in ("secure", "nonsecure"):
        raise ValueError(f'a probe state is "secure", "nonsecure" or None, not {state!r}')
    if "secure" not in registers:
        raise ConfigError(
            "a probe state needs a machine with TrustZone enabled",
            file="probe",
            key="state",
            expected="no state on a machine without TrustZone enabled",
        )
    return state == "secure"
