from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class SupplyOptions:
    """Supply voltages of the board, shared by every analog peripheral of the MCU.

    Attributes:
        vdda_mv: VDDA in millivolts, 1620 to 3600, also VREF+: the reference of every ADC
            conversion. 3000 is the voltage the factory calibration words were taken at.
    """

    vdda_mv: int = 3000
