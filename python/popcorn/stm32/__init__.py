"""STM32 parts: descriptions shipped with popcorn or built from ST's files.

Imports nothing native at package import: popcorn's build runs `popcorn.stm32.description` in an
interpreter without `popcorn._core`.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from popcorn._part_description import PartDescription


def mcu(part: str, *, core: str | None = None, svd: Path | None = None) -> PartDescription:
    """The description of an STM32 part: from a directory of `POPCORN_STM32_DESCRIPTION_DIRS`
    when one holds it, else the description shipped with popcorn, else one built from ST's
    files on first use and cached. The shipped one is skipped when `svd` is given or
    `POPCORN_STM32_BUILD` is `local`. A description directory wins even over `svd`.

    Args:
        part: Part number, such as `STM32U575QGIx`. A CPN matches a RefName's `x` positions.
        core: Core of a multi-core part.
        svd: SVD file to build from instead of the one of the local ST install.

    Raises:
        ConfigError: The part is unknown, with close matches, or `POPCORN_STM32_BUILD` is set
            to anything but `local`.
        MissingInput: The part is not shipped and ST's files are missing. The message names
            the shipped parts and the missing path.
    """
    from popcorn._part_description import load_description
    from popcorn.stm32.description import description_path

    return load_description(description_path(part, core=core, svd=svd))
