"""STM32 parts: descriptions built from ST's files.

Imports nothing native at package import: popcorn's build runs `popcorn.stm32.description` in an
interpreter without `popcorn._core`.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from popcorn._description import PartDescription


def mcu(part: str, *, core: str | None = None, svd: Path | None = None) -> PartDescription:
    """The description of an STM32 part, built from ST's files on first use and cached.

    Args:
        part: Part number, such as `STM32U575QGIx`.
        core: Core of a multi-core part.
        svd: SVD file to use instead of the one of the local ST install.

    Raises:
        ConfigError: The part is unknown, with close matches, or ST's files are missing.
    """
    from popcorn._description import load_description
    from popcorn.stm32.description import cached_description

    return load_description(cached_description(part, core=core, svd=svd))
