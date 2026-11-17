"""Names a profile report gives addresses, exception numbers and register ids."""

from __future__ import annotations

import bisect
from collections.abc import Iterable, Sequence
from dataclasses import dataclass

from popcorn._image import Image
from popcorn._part_description import PartDescription

SYSTEM_EXCEPTIONS = {
    1: "Reset",
    2: "NMI",
    3: "HardFault",
    4: "MemManage",
    5: "BusFault",
    6: "UsageFault",
    7: "SecureFault",
    11: "SVCall",
    12: "DebugMonitor",
    14: "PendSV",
    15: "SysTick",
}
"""Cortex-M system exception names by exception number."""

OVERFLOWED_REGISTER = 0xFFFF
"""Register id of an access whose flat id did not fit a trace record."""


@dataclass(frozen=True, slots=True)
class _Range:
    start: int
    end: int
    name: str


def _sorted(ranges: Iterable[_Range]) -> tuple[_Range, ...]:
    return tuple(sorted(ranges, key=lambda r: (r.start, r.end)))


def _find(ranges: Sequence[_Range], starts: Sequence[int], address: int) -> _Range | None:
    at = bisect.bisect_right(starts, address)
    if at == 0:
        return None
    found = ranges[at - 1]
    return found if address < found.end else None


def _alias_name(names: Sequence[str]) -> str:
    if len(names) == 1:
        return names[0]
    return f"{names[0]} (+{len(names) - 1} alias{'es' if len(names) > 2 else ''})"


class Names:
    """Function, exception and register names of one machine, read once at the profile start.

    Functions come from the function symbols of the loaded and symbols-only images, as the call
    stack tracker reads them. Symbols with one start and one size, weak handlers aliased to one
    default handler for example, make one function named by the first of them in image and
    symbol order, with the count of the others: `ADC1_IRQHandler (+80 aliases)`. Exceptions 1
    to 15 have their Cortex-M names, an interrupt the name of its description entry without
    `_IRQn`. Registers are `<peripheral>.<register>` by flat register id, in description order.
    """

    __slots__ = (
        "_addresses",
        "_function_starts",
        "_functions",
        "_irqs",
        "_ranges",
        "_register_starts",
        "_registers",
    )

    def __init__(self, images: Iterable[Image], description: PartDescription) -> None:
        """Index the function symbols of `images` and the registers of `description`."""
        aliases: dict[tuple[int, int], list[str]] = {}
        for image in images:
            for symbol in image.symbols:
                if symbol.function and symbol.size != 0:
                    named = aliases.setdefault((symbol.value, symbol.value + symbol.size), [])
                    if symbol.name not in named:
                        named.append(symbol.name)
        self._functions = _sorted(
            _Range(start, end, _alias_name(named)) for (start, end), named in aliases.items()
        )
        self._function_starts = [r.start for r in self._functions]
        self._irqs = {irq.number + 16: irq.name.removesuffix("_IRQn") for irq in description.irqs}
        names: list[str] = []
        addresses: list[int] = []
        ranges: list[_Range] = []
        for peripheral in description.peripherals:
            for register in peripheral.registers:
                name = f"{peripheral.name}.{register.name}"
                start = peripheral.base + register.offset
                names.append(name)
                addresses.append(start)
                ranges.append(_Range(start, start + (register.width + 7) // 8, name))
        self._registers = tuple(names)
        self._addresses = tuple(addresses)
        self._ranges = _sorted(ranges)
        self._register_starts = [r.start for r in self._ranges]

    def function(self, pc: int) -> tuple[str, int]:
        """Name and start of the function covering `pc`, or its address and `pc` when none
        does."""
        found = _find(self._functions, self._function_starts, pc)
        if found is None:
            return f"{pc:#010x}", pc
        return found.name, found.start

    def location(self, pc: int) -> str:
        """`function+offset` of `pc`, `function` at its start, its address when no function
        covers it."""
        found = _find(self._functions, self._function_starts, pc)
        if found is None:
            return f"{pc:#010x}"
        return found.name if pc == found.start else f"{found.name}+{pc - found.start:#x}"

    def function_at(self, start: int) -> str:
        """Name of the function starting at `start`, its address when none does."""
        name, found = self.function(start)
        return name if found == start else f"{start:#010x}"

    def exception(self, number: int) -> str:
        """Name of an exception number."""
        if number in SYSTEM_EXCEPTIONS:
            return SYSTEM_EXCEPTIONS[number]
        return self._irqs.get(number, f"exception {number}")

    def register_address(self, flat_id: int, address: int) -> int:
        """Address of a register by flat id, `address` when the id overflowed or is
        unknown."""
        if flat_id != OVERFLOWED_REGISTER and flat_id < len(self._addresses):
            return self._addresses[flat_id]
        return address

    def register(self, flat_id: int, address: int) -> str:
        """Name of a register by flat id, by address when the id overflowed or is unknown."""
        if flat_id != OVERFLOWED_REGISTER and flat_id < len(self._registers):
            return self._registers[flat_id]
        found = _find(self._ranges, self._register_starts, address)
        return found.name if found is not None else f"{address:#010x}"
