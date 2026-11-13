"""Stop and fault reports: why a machine stopped, its fault registers decoded, its call stack.

    stop = await m.run(budget=1_000_000)
    if stop.reason is not popcorn.StopReason.USER:
        print(popcorn.report.stop_report(m, stop).text())

The call stack walks epilogues, prologues and exception frames exactly, without CFI. Where
that fails, the rest comes from a scan of the stack for return addresses that follow a call,
frames marked heuristic. Unwinding reads the description's memory regions only, never a
peripheral window, so taking a report changes no peripheral state.

`exception_stop(error)` is the first stop an exception tree carries, None without one.
`exception_stops(error)` lists every stop of the tree in walk order, for an error that
carries several, such as an `ExceptionGroup` of two machines' faults.

Limits:

- The report reads the machine as it is: take it right after the stop, before anything else
  runs. A fault inside a `Machine.call` is the exception: its stop carries the registers taken
  before the call restored the caller's, and the report shows and unwinds those, over the
  stack memory as it is when the report is taken. Build it before running the machine again:
  with `stack=CURRENT`, the caller's later pushes overwrite the faulted frames.
- Prologues are decoded from the function's first instruction. A shrink-wrapped function, one
  that saves registers only on some paths after its entry, can be misread while pc is on a path
  that skipped the save.
- Exception frames on the Non-secure stack or with additional state context are scanned: the
  exact walk stops there and the stack scan takes over. Without TrustZone popcorn runs the
  core Secure, so these frames never occur. With TrustZone they are the frames of an exception
  taken across the security states.
- A Non-secure function a Secure BLXNS called returns to FNC_RETURN: the walk goes on in the
  Secure caller from the frame BLXNS pushed on the Secure stack. The other way, a Secure
  function an SG veneer entered returning to its Non-secure caller, ends the walk.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import cast, override

from popcorn import _core
from popcorn._enums import FaultKind
from popcorn._errors import EmulationError
from popcorn._image import Location
from popcorn._machine import Machine, stack_access
from popcorn._stop import Stop
from popcorn._unwind import RawFrame, Read32, Via, source_address, unwind

_CORE_REGISTERS = (
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r5",
    "r6",
    "r7",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "sp",
    "lr",
    "pc",
    "xpsr",
)


_CFSR = 0xE000_ED28
_SFAR = 0xE000_EDE8
_SFARVALID = 6


def _group(bit: int) -> str:
    if bit < 8:
        return "MMFSR"
    if bit < 16:
        return "BFSR"
    return "UFSR"


def fault_bits(cfsr: int, hfsr: int, sfsr: int = 0) -> dict[str, tuple[str, ...]]:
    """Names of the bits set in CFSR, split into MMFSR, BFSR and UFSR, in HFSR, and in the
    SFSR of a core with TrustZone enabled.

    Only registers with a named bit set appear. Names are the Armv8-M ARM's.
    """
    groups: dict[str, list[str]] = {}
    for bit in _core.CfsrBit:
        if cfsr >> bit.value & 1:
            groups.setdefault(_group(bit.value), []).append(bit.name.upper())
    for bit in _core.HfsrBit:
        if hfsr >> bit.value & 1:
            groups.setdefault("HFSR", []).append(bit.name.upper())
    for bit in _core.SfsrBit:
        if sfsr >> bit.value & 1:
            groups.setdefault("SFSR", []).append(bit.name.upper())
    return {name: tuple(bits) for name, bits in groups.items()}


@dataclass(frozen=True, slots=True)
class StackFrame:
    """One frame of a call stack.

    Attributes:
        pc: Address running in the frame, or returned to in its caller.
        location: Symbol, file and line: of pc for the innermost frame and exception frames, of
            the call instruction for a return address.
        via: How the frame was found.
    """

    pc: int
    location: Location
    via: Via

    @property
    def exact(self) -> bool:
        """Whether the frame is certain, not found by the stack scan."""
        return self.via.exact

    @override
    def __str__(self) -> str:
        where = self.location
        text = f"{self.pc:#010x}"
        if where.symbol:
            text += f" {where.symbol}+{where.offset:#x}"
        if where.file:
            text += f" {where.file}:{where.line}"
        return text if self.exact else f"{text} (heuristic)"


@dataclass(frozen=True, slots=True)
class Report:
    """A stop explained, `text()` printing it for a test log.

    Attributes:
        stop: The stop.
        registers: Core registers when the report was taken, or those the stop carries for a
            fault inside a `Machine.call`.
        fault_bits: Fault status bits set, by register, when the stop is a fault. SFSR only for
            a SecureFault, whose CFSR is read from the SCS: SFSR and CFSR are sticky, so for a
            SecureFault the CFSR may hold bits of an earlier fault the handler did not clear.
        frames: The call stack, innermost first.
        ring: The trace ring's last records, or why it could not be read.
        sfar: SFAR when the fault is a SecureFault whose SFSR has SFARVALID set, else None.
    """

    stop: Stop
    registers: Mapping[str, int]
    fault_bits: Mapping[str, tuple[str, ...]] = field(default_factory=dict[str, tuple[str, ...]])
    frames: tuple[StackFrame, ...] = ()
    ring: str = ""
    sfar: int | None = None

    def text(self) -> str:
        """The report as lines of text."""
        stop = self.stop
        lines = [
            f"stop: {stop.reason.name} at {stop.location}",
            f"  instructions {stop.instructions}, time {stop.time.seconds:.9f} s",
        ]
        if stop.detail:
            lines.append(f"  detail: {stop.detail}")
        if stop.fault is not None:
            fault = stop.fault
            status = "SFSR" if fault.kind is FaultKind.SECURE else "CFSR"
            lines.append(
                f"fault: {fault.kind.name} address {fault.address:#010x} "
                f"{status} {fault.cfsr:#010x} HFSR {fault.hfsr:#010x}"
            )
            lines.extend(f"  {name}: {' '.join(bits)}" for name, bits in self.fault_bits.items())
            if self.sfar is not None:
                lines.append(f"  SFAR: {self.sfar:#010x}")
        lines.append("registers:")
        names = list(self.registers)
        for row in range(0, len(names), 4):
            cells = [f"{name:>4} {self.registers[name]:#010x}" for name in names[row : row + 4]]
            lines.append("  " + "  ".join(cells))
        lines.append("call stack:")
        lines.extend(f"  #{index} {frame}" for index, frame in enumerate(self.frames))
        if self.ring:
            lines.append("trace ring:")
            lines.extend(f"  {line}" for line in self.ring.splitlines())
        return "\n".join(lines)


def stop_report(
    machine: Machine,
    stop: Stop,
    *,
    ring: bool = True,
    ring_last: int | None = None,
    collapse: bool = False,
) -> Report:
    """Explain `stop` from the state of `machine`, which must be stopped, and from the
    registers `stop` carries for a fault inside a `Machine.call`, the machine's own standing
    in for any it lacks.

    Args:
        machine: The machine, stopped where `stop` left it.
        stop: The stop to explain.
        ring: Whether to include the trace ring. When the ring cannot be written or read, the
            report says why instead of raising, so the ring never hides the failure it reports.
        ring_last: The ring's last lines to keep, as `Ring.dump(last=)` takes it.
        collapse: Whether to collapse repeated record groups, as `Ring.dump(collapse=)`.

    Raises:
        MachineRunning: The machine is busy, outside its handlers.
    """
    captured: Mapping[str, int] = stop.registers or {}

    def register(name: str) -> int:
        value = captured.get(name)
        return int(getattr(machine.reg, name)) if value is None else value

    registers = {name: register(name) for name in _CORE_REGISTERS}
    frames = call_stack(machine, captured)
    bits: dict[str, tuple[str, ...]] = {}
    sfar = None
    if stop.fault is not None:
        fault = stop.fault
        secure = fault.kind is FaultKind.SECURE
        sfsr = fault.cfsr if secure else 0
        cfsr = machine.mem.u32[_CFSR] if secure else fault.cfsr
        bits = fault_bits(cfsr, fault.hfsr, sfsr)
        if sfsr >> _SFARVALID & 1:
            sfar = machine.mem.u32[_SFAR]
    text = ""
    if ring:
        try:
            text = machine.ring.dump(last=ring_last, collapse=collapse)
        except OSError as error:
            text = f"ring unavailable: {error}"
    return Report(stop, registers, bits, frames, text, sfar)


def call_stack(
    machine: Machine, captured: Mapping[str, int] | None = None
) -> tuple[StackFrame, ...]:
    """The call stack of `machine`, innermost first, from its registers, `captured` standing in
    for any it holds, with the source location of every frame.

    Args:
        machine: The machine, stopped.
        captured: Register values to use in place of the machine's own, by name.

    Raises:
        MachineRunning: The machine is busy, outside its handlers.
    """
    held: Mapping[str, int] = captured or {}

    def register(name: str) -> int:
        value = held.get(name)
        return int(getattr(machine.reg, name)) if value is None else value

    read32, functions = stack_access(machine)
    raw = unwind(
        read32,
        functions,
        register("pc"),
        register("lr"),
        register("sp"),
        register("psp"),
        _secure_sp(machine, register("xpsr")),
    )
    return tuple(_frame(machine, read32, f) for f in raw)


def _secure_sp(machine: Machine, xpsr: int) -> int | None:
    """The Secure stack pointer of the core's mode while it runs Non-secure, where a BLXNS
    pushed its return frame, DDI0553B.z B3.16."""
    if machine.security_state != "nonsecure":
        return None
    regs = machine.reg
    thread = xpsr & 0x1FF == 0
    return int(regs.psp_s) if thread and int(regs.control_s) >> 1 & 1 else int(regs.msp_s)


def _frame(machine: Machine, read32: Read32, raw: RawFrame) -> StackFrame:
    return StackFrame(raw.pc, machine.locate(source_address(read32, raw)), raw.via)


def exception_stops(error: BaseException | None) -> tuple[Stop, ...]:
    """Every stop an exception tree carries, in walk order.

    An `EmulationError` whose `stop` is not None carries one. The walk is depth first. An error's
    own stop comes before its members and its chain. The members of an `ExceptionGroup` come
    first, in `exceptions` order, then the exception's own chain: its
    `__cause__` when set, else its `__context__`, one of the two. With both set the context is
    not visited, and `raise X from None` still follows `__context__`. An exception is visited
    once, so a cycle ends. No error, or no stop in the tree, gives the empty tuple.
    """
    found: list[Stop] = []
    seen: set[int] = set()
    pending: list[BaseException | None] = [error]
    while pending:
        current = pending.pop()
        if current is None or id(current) in seen:
            continue
        seen.add(id(current))
        if isinstance(current, EmulationError) and current.stop is not None:
            found.append(current.stop)
        pending.append(current.__cause__ or current.__context__)
        if isinstance(current, BaseExceptionGroup):
            group = cast(BaseExceptionGroup[BaseException], current)
            pending.extend(reversed(group.exceptions))
    return tuple(found)


def exception_stop(error: BaseException | None) -> Stop | None:
    """The first stop of `exception_stops`, None when the tree carries none."""
    stops = exception_stops(error)
    return stops[0] if stops else None


__all__ = [
    "Report",
    "StackFrame",
    "Via",
    "exception_stop",
    "exception_stops",
    "fault_bits",
    "stop_report",
]
