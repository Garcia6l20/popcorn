"""Part descriptions: the native reflected aggregate, read from JSON."""

from __future__ import annotations

import os

from popcorn import _core

PartDescription = _core.PartDescription
"""What a part is: core, memory map, peripherals and interrupts."""


def load_description(path: str | os.PathLike[str]) -> PartDescription:
    """Read an MCU description JSON file, ConfigError naming the file and key when invalid."""
    return _core.load_description(os.fspath(path))


def parse_description(text: str, source: str = "<string>") -> PartDescription:
    """Parse MCU description JSON text, `source` naming it in errors."""
    return _core.parse_description(text, source)


def description_digest(description: PartDescription) -> str:
    """SHA-256 of the canonical JSON of a description, what snapshots record."""
    return _core.description_digest(description)
