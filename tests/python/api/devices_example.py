"""Bus devices written in Python for the device tests and the project file examples."""

from __future__ import annotations

from popcorn import ResetKind
from popcorn.board import I2CTarget, SpiTarget
from popcorn.units import VirtualTime, ms


class Eeprom24(I2CTarget):
    """A 24xx EEPROM: two word address bytes, 16-byte pages, the address NACKed during tWR."""

    pointer: int
    writing: bool
    written: int
    busy_until: int
    array: bytearray
    pending: bytearray

    def __init__(self, address: int, size: int, page: int = 16) -> None:
        if size <= 0 or size % page:
            raise ValueError("an EEPROM size is a positive multiple of its page")
        super().__init__(address)
        self.size = size
        self.page = page
        self.array = bytearray(b"\xff" * size)
        self.pointer = 0
        self.writing = False
        self.written = 0
        self.busy_until = 0
        self.pending = bytearray()
        self.resets: list[ResetKind] = []
        self.nacks = 0

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        if at.ps < self.busy_until:
            self.nacks += 1
            return False
        self.writing = not read
        self.written = 0
        self.pending = bytearray()
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        if self.written < 2:
            self.pointer = ((self.pointer << 8) | byte) % self.size if self.written else byte
            self.written += 1
            return True
        self.pending.append(byte)
        return True

    def on_read(self, at: VirtualTime) -> int:
        value = self.array[self.pointer]
        self.pointer = (self.pointer + 1) % self.size
        return value

    def on_stop(self, at: VirtualTime) -> None:
        if self.writing and self.pending:
            base = self.pointer & ~(self.page - 1)
            for index, value in enumerate(self.pending):
                self.array[base + (self.pointer - base + index) % self.page] = value
            self.busy_until = at.ps + ms(5).ps
        self.writing = False
        self.pending = bytearray()

    def reset(self, kind: ResetKind) -> None:
        self.resets.append(kind)
        if kind in (ResetKind.POWER_ON, ResetKind.BROWN_OUT):
            self.busy_until = 0
            self.pointer = 0
            self.writing = False
            self.written = 0
            self.pending = bytearray()


class JedecDevice(SpiTarget):
    """Answers the 9Fh JEDEC ID instruction with a fixed ID, all ones otherwise."""

    position: int
    command: int

    def __init__(
        self, cs: str | None = None, jedec: tuple[int, int, int] = (0x1F, 0x32, 0x17)
    ) -> None:
        super().__init__(cs)
        self.jedec = jedec
        self.position = 0
        self.command = 0
        self.selects: list[bool] = []
        self.resets: list[ResetKind] = []

    def on_select(self, selected: bool, at: VirtualTime) -> None:
        self.selects.append(selected)
        self.position = 0

    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        if self.position == 0:
            self.command = out
        reply = 0xFF
        if self.command == 0x9F and 1 <= self.position <= 3:
            reply = self.jedec[self.position - 1]
        self.position += 1
        return reply

    def reset(self, kind: ResetKind) -> None:
        self.resets.append(kind)
