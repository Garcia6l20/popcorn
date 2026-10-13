"""The interrupt controller from the debugger's side, and the description's IRQ table."""

from __future__ import annotations

import difflib

from popcorn._part_description import PartDescription
from popcorn._views import Memory

_ISER = 0xE000_E100
_ICER = 0xE000_E180
_ISPR = 0xE000_E200
_ICPR = 0xE000_E280


class Nvic:
    """Pend, clear and inspect external interrupts, `m.nvic.pend(m.irq.USART2_IRQn)`.

    Debugger writes and reads of the NVIC registers through `m.mem`: the firmware takes a
    pending, enabled interrupt when it next runs with interrupts unmasked.
    """

    __slots__ = ("_count", "_memory")

    def __init__(self, memory: Memory, count: int) -> None:
        """View the NVIC of a core with `count` external interrupts."""
        self._memory = memory
        self._count = count

    def pend(self, irq: int) -> None:
        """Set interrupt `irq` pending."""
        self._set(_ISPR, irq)

    def unpend(self, irq: int) -> None:
        """Clear the pending state of interrupt `irq`."""
        self._set(_ICPR, irq)

    def enable(self, irq: int) -> None:
        """Enable interrupt `irq`, as the firmware's NVIC_EnableIRQ does."""
        self._set(_ISER, irq)

    def disable(self, irq: int) -> None:
        """Disable interrupt `irq`."""
        self._set(_ICER, irq)

    def pending(self, irq: int) -> bool:
        """Whether interrupt `irq` is pending."""
        return self._bit(_ISPR, irq)

    def enabled(self, irq: int) -> bool:
        """Whether interrupt `irq` is enabled."""
        return self._bit(_ISER, irq)

    def _set(self, base: int, irq: int) -> None:
        self._memory.u32[self._word(base, irq)] = 1 << (irq % 32)

    def _bit(self, base: int, irq: int) -> bool:
        return bool(self._memory.u32[self._word(base, irq)] >> (irq % 32) & 1)

    def _word(self, base: int, irq: int) -> int:
        if not 0 <= irq < self._count:
            raise ValueError(f"interrupt {irq} is not below the core's {self._count}")
        return base + 4 * (irq // 32)


class Irqs:
    """Interrupt numbers of the description by name, `m.irq.USART2_IRQn`."""

    __slots__ = ("_numbers",)

    def __init__(self, description: PartDescription) -> None:
        """Index the description's interrupt table."""
        self._numbers = {irq.name: irq.number for irq in description.irqs}

    def __getattr__(self, name: str) -> int:
        number = self._numbers.get(name)
        if number is None:
            raise AttributeError(self._missing(name))
        return number

    def __getitem__(self, name: str) -> int:
        number = self._numbers.get(name)
        if number is None:
            raise KeyError(self._missing(name))
        return number

    def _missing(self, name: str) -> str:
        close = difflib.get_close_matches(name, list(self._numbers))
        hint = f", close matches: {', '.join(close)}" if close else ""
        return f"the description has no interrupt {name}{hint}"

    def names(self) -> tuple[str, ...]:
        """Interrupt names, in description order."""
        return tuple(self._numbers)

    def __dir__(self) -> list[str]:
        return [*self._numbers]
