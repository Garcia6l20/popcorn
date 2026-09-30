"""Stand-in model classes the project file tests reference."""

from popcorn.board import I2CTarget
from popcorn.model import Model


class Eeprom(I2CTarget):
    """A bus device."""

    def __init__(self, address: int, size: int) -> None:
        super().__init__(address)
        self.size = size


class Timer(Model):
    """A peripheral model."""

    def __init__(self, prescaler: int = 1) -> None:
        super().__init__()
        self.prescaler = prescaler


class Plain:
    """A class that is not a model."""


NOT_A_CLASS = 3
