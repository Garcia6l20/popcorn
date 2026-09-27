"""Python models of the model test description: a periodic timer and an I2C-style EEPROM."""

from __future__ import annotations

from popcorn.model import IrqLine, Model, on_read, on_write
from popcorn.units import VirtualTime, us


class Timer(Model):
    """Counts expirations of a period of ARR microseconds while CR.EN is set."""

    irq: IrqLine
    expirations: int = 0

    def __init__(self) -> None:
        super().__init__()
        self.writes: list[tuple[int, int]] = []
        self.fired: list[VirtualTime] = []

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        self.writes.append((old, new))
        if new & self.fields.CR.EN and not old & self.fields.CR.EN:
            self.schedule(us(self.regs.ARR), self._expire)

    @on_write("SR")
    def _sr(self, old: int, new: int) -> None:
        if not new & self.fields.SR.UIF:
            self.irq.lower()

    @on_read("CNT")
    def _cnt(self) -> int:
        return self.expirations

    def reset_count(self) -> None:
        self.expirations = 0

    def _expire(self) -> None:
        self.expirations += 1
        self.fired.append(self.now)
        self.regs.SR |= self.fields.SR.UIF
        self.irq.raise_()
        if self.regs.CR & self.fields.CR.EN:
            self.schedule(us(self.regs.ARR), self._expire)


class Eeprom(Model):
    """A 256-byte EEPROM behind START and STOP conditions and a data register."""

    device = 0xA0
    memory: bytearray
    pointer: int = 0
    phase: str = "idle"

    def __init__(self) -> None:
        super().__init__()
        self.memory = bytearray(256)

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        if new & self.fields.CR.START:
            self.phase = "address"
        elif new & self.fields.CR.STOP:
            self.phase = "idle"

    @on_write("DR")
    def _dr(self, old: int, new: int) -> None:
        if self.phase == "address" and new & 0xFE == self.device:
            self.phase = "read" if new & 1 else "pointer"
        elif self.phase == "pointer":
            self.pointer, self.phase = new, "data"
        elif self.phase == "data":
            self.memory[self.pointer] = new
            self.pointer = (self.pointer + 1) % len(self.memory)

    @on_read("DR")
    def _data(self) -> int:
        value = self.memory[self.pointer]
        self.pointer = (self.pointer + 1) % len(self.memory)
        return value
