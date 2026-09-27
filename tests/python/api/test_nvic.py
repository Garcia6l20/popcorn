"""The NVIC from the debugger's side and the description's IRQ table."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn.units import us


def test_pend_runs_the_firmware_handler(board: popcorn.Machine) -> None:
    irq = board.irq.WWDG_IRQn
    assert irq == 0 and board.nvic.enabled(irq)
    board.nvic.pend(irq)
    assert board.nvic.pending(irq)
    asyncio.run(board.run(for_=us(10)))
    assert board.mem.u32[board.sym("wwdg_entries")] == 1
    assert not board.nvic.pending(irq)


def test_a_disabled_interrupt_stays_pending(board: popcorn.Machine) -> None:
    irq = board.irq["WWDG_IRQn"]
    board.nvic.disable(irq)
    board.nvic.pend(irq)
    asyncio.run(board.run(for_=us(10)))
    assert board.mem.u32[board.sym("wwdg_entries")] == 0
    board.nvic.unpend(irq)
    assert not board.nvic.pending(irq)
    board.nvic.enable(irq)
    assert board.nvic.enabled(irq)


def test_irq_names_and_ranges(board: popcorn.Machine) -> None:
    usart1 = board.irq.USART1_IRQn
    assert usart1 == 61
    board.nvic.pend(usart1)
    assert board.nvic.pending(usart1) and not board.nvic.pending(usart1 - 32)
    assert board.mem.u32[0xE000_E204] == 1 << 29
    board.nvic.unpend(usart1)
    assert not board.nvic.pending(usart1)
    assert "USART1_IRQn" in board.irq.names()
    with pytest.raises(AttributeError, match="USART1_IRQn"):
        _ = board.irq.USART1_IRQ
    with pytest.raises(KeyError, match="close matches: USART1_IRQn"):
        _ = board.irq["USART1_IRQ"]
    with pytest.raises(ValueError, match="not below"):
        board.nvic.pend(board.description.core.num_irqs)
