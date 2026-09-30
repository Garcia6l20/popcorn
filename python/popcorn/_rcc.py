from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class RccOptions:
    """Clock sources the board gives the built-in STM32 RCC model.

    Attributes:
        hse_hz: HSE crystal or bypass clock frequency, 4 to 50 MHz, RM0456 section 11.4.
        lse_hz: LSE crystal or bypass clock frequency, above 0 Hz and at most 1 MHz.
    """

    hse_hz: int = 16_000_000
    lse_hz: int = 32_768
