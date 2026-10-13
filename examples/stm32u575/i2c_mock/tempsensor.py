"""A temperature sensor on I2C, written in Python as a `popcorn.board.I2CTarget`.

The register map follows the common TMP102 layout: a pointer register selects the 16-bit
register a transfer reads or writes, 0 the temperature, 1 the configuration. The temperature is
12 bits of 1/16 degree, left-aligned, MSB first.
"""

from collections.abc import Sequence

from popcorn import ResetKind
from popcorn.board import I2CTarget
from popcorn.units import VirtualTime

TEMPERATURE = 0
CONFIG = 1


def counts(celsius: float) -> int:
    """The 12-bit temperature register value of `celsius`, 1/16 degree per count."""
    return round(celsius * 16) & 0xFFF


class TempSensor(I2CTarget):
    """A pointer-register temperature sensor answering scripted readings.

    Each read of the temperature register takes the next reading of `readings`, the last one
    repeating once they are used up. The annotated fields are the state a snapshot keeps.
    `readings` is not one of them: it is the script, fixed when the sensor is built and
    read-only, because the `machine` fixture reuses one sensor for every test of a config. A
    test wanting other readings uses a config of `MachineDescription.with_configs`, or builds its
    own
    machine with `Machine(devices={"I2C1": [TempSensor(0x48, readings=...)]})`.

    Fault injection: while `nack_starts` is above 0, the sensor NACKs its address and counts it
    down. It is a state field, so the fixture's restore clears it before the next test.
    """

    pointer: int
    config: int
    config_writes: int
    served: int
    nack_starts: int
    written: int
    pending: int

    def __init__(self, address: int, readings: Sequence[float] = (20.0,)) -> None:
        """Answer `address` with `readings`, in degrees Celsius."""
        super().__init__(address)
        self._readings = tuple(counts(value) for value in readings)
        self.pointer = TEMPERATURE
        self.config = 0
        self.config_writes = 0
        self.served = 0
        self.nack_starts = 0
        self.written = 0
        self.pending = 0

    @property
    def readings(self) -> tuple[int, ...]:
        """The scripted temperature register values, in counts."""
        return self._readings

    def reset(self, kind: ResetKind) -> None:
        """Clear the registers on POWER_ON and BROWN_OUT. The script and `served` stay."""
        if kind in (ResetKind.POWER_ON, ResetKind.BROWN_OUT):
            self.pointer = TEMPERATURE
            self.config = 0
            self.written = 0
            self.pending = 0

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        if self.nack_starts > 0:
            self.nack_starts -= 1
            return False
        self.written = 0
        if read and self.pointer == TEMPERATURE:
            reading = self._readings[min(self.served, len(self._readings) - 1)]
            self.served += 1
            self.pending = reading << 4
        elif read:
            self.pending = self.config
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        if self.written == 0:
            self.pointer = byte
        elif self.pointer == CONFIG:
            self.config = (self.config << 8 | byte) & 0xFFFF
            if self.written == 2:
                self.config_writes += 1
        self.written += 1
        return True

    def on_read(self, at: VirtualTime) -> int:
        byte = self.pending >> 8
        self.pending = self.pending << 8 & 0xFFFF
        return byte
