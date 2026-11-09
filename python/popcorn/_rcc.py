from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class RccOptions:
    """Clock sources the board gives the built-in STM32 RCC model.

    Attributes:
        hse_hz: HSE crystal or bypass clock frequency, 4 to 50 MHz, RM0456 section 11.4.
        lse_hz: LSE crystal or bypass clock frequency, above 0 Hz and at most 1 MHz.
        lsi_hz: LSI frequency in Hz, ``None`` for the family nominal: 32 kHz on the STM32U5,
            STM32H5 and STM32L4, 37 kHz on the STM32L1. The datasheet full-range limits bound it,
            30.4 to 33.6 kHz on the STM32U5, 29.4 to 33.6 kHz on the STM32H5, 29.5 to 34 kHz on the
            STM32L4 and 26 to 56 kHz on the STM32L1. On the STM32U5 ``LSIPREDIV`` divides it.
    """

    hse_hz: int = 16_000_000
    lse_hz: int = 32_768
    lsi_hz: int | None = None
