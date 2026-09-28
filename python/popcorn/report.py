"""Stop and fault reports: why a machine stopped, its fault registers decoded, its call stack.

    stop = await m.run(budget=1_000_000)
    if stop.reason is not popcorn.StopReason.USER:
        print(popcorn.report.stop_report(m, stop).text())

The call stack walks epilogues, prologues and exception frames exactly, without CFI. Where
that fails, the rest comes from a scan of the stack for return addresses that follow a call,
frames marked heuristic. Unwinding reads the description's memory regions only, never a
peripheral window, so taking a report changes no peripheral state.

Limits:

- The report reads the machine as it is: take it right after the stop, before anything else
  runs. A `Machine.call` restores the caller's context when it ends, so a fault inside a call
  reports the call's stop but the caller's registers and stack.
- Prologues are decoded from the function's first instruction. A shrink-wrapped function, one
  that saves registers only on some paths after its entry, can be misread while pc is on a path
  that skipped the save.
- Exception frames on the Non-secure stack or with additional state context are scanned: the
  exact walk stops there and the stack scan takes over.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field

from popcorn import _core
from popcorn._errors import EmulationError
from popcorn._image import Location
from popcorn._machine import Machine
from popcorn._stop import Stop
from popcorn._unwind import Functions, RawFrame, Read32, Via, source_address, unwind

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


def _group(bit: int) -> str:
    if bit < 8:
        return "MMFSR"
    if bit < 16:
        return "BFSR"
    return "UFSR"


def fault_bits(cfsr: int, hfsr: int) -> dict[str, tuple[str, ...]]:
    """Names of the bits set in CFSR, split into MMFSR, BFSR and UFSR, and in HFSR.

    Only registers with a named bit set appear. Names are the Armv8-M ARM's.
    """
    groups: dict[str, list[str]] = {}
    for bit in _core.CfsrBit:
        if cfsr >> bit.value & 1:
            groups.setdefault(_group(bit.value), []).append(bit.name.upper())
    for bit in _core.HfsrBit:
        if hfsr >> bit.value & 1:
            groups.setdefault("HFSR", []).append(bit.name.upper())
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
        registers: Core registers when the report was taken.
        fault_bits: Fault status bits set, by register, when the stop is a fault.
        frames: The call stack, innermost first.
        ring: The trace ring's last records, or why it could not be read.
    """

    stop: Stop
    registers: Mapping[str, int]
    fault_bits: Mapping[str, tuple[str, ...]] = field(default_factory=dict[str, tuple[str, ...]])
    frames: tuple[StackFrame, ...] = ()
    ring: str = ""

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
            lines.append(
                f"fault: {fault.kind.name} address {fault.address:#010x} "
                f"CFSR {fault.cfsr:#010x} HFSR {fault.hfsr:#010x}"
            )
            lines.extend(f"  {name}: {' '.join(bits)}" for name, bits in self.fault_bits.items())
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


def stop_report(machine: Machine, stop: Stop, *, ring: bool = True) -> Report:
    """Explain `stop` from the state of `machine`, which must be stopped.

    Args:
        machine: The machine, stopped where `stop` left it.
        stop: The stop to explain.
        ring: Whether to include the trace ring. When the ring cannot be written or read, the
            report says why instead of raising, so the ring never hides the failure it reports.

    Raises:
        MachineRunning: A run is active.
    """
    registers = {name: getattr(machine.reg, name) for name in _CORE_REGISTERS}

    regions = [(r.start, r.start + r.size) for r in machine.description.regions]

    def read32(address: int) -> int | None:
        if not any(start <= address and address + 4 <= end for start, end in regions):
            return None
        try:
            return machine.mem.u32[address]
        except EmulationError:
            return None

    functions = Functions((s.name, s.value, s.size) for s in machine.symbols.values() if s.function)
    raw = unwind(
        read32,
        functions,
        registers["pc"],
        registers["lr"],
        registers["sp"],
        machine.reg.psp,
    )
    frames = tuple(_frame(machine, read32, f) for f in raw)
    bits = {} if stop.fault is None else fault_bits(stop.fault.cfsr, stop.fault.hfsr)
    text = ""
    if ring:
        try:
            text = machine.ring.dump()
        except OSError as error:
            text = f"ring unavailable: {error}"
    return Report(stop, registers, bits, frames, text)


def _frame(machine: Machine, read32: Read32, raw: RawFrame) -> StackFrame:
    return StackFrame(raw.pc, machine.locate(source_address(read32, raw)), raw.via)


__all__ = ["Report", "StackFrame", "Via", "fault_bits", "stop_report"]
