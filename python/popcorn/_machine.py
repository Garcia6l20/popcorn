"""The emulated machine."""

from __future__ import annotations

import os
from collections.abc import Iterable

from popcorn import _core
from popcorn._description import MachineDescription, load_description
from popcorn._enums import ResetKind
from popcorn._image import Image, Location, location_of


class Machine:
    """One emulated MCU: a description, the images loaded into it and its run state.

    Built powered off: nothing runs until `reset()`.
    """

    __slots__ = ("_description", "_handle", "_images")

    def __init__(
        self,
        description: MachineDescription | str | os.PathLike[str],
        *,
        image: Image | Iterable[Image] = (),
        core_hz: int = 0,
        cpi: float = 1.0,
        trace_capacity: int = 4096,
        spin_threshold: int = 0,
        strict_peripherals: bool = False,
        vector_table: int | None = None,
    ) -> None:
        """Build a machine from a description or the path of its JSON, then load `image`.

        Args:
            description: The description, or the path of its JSON file.
            image: Image or images to load, in order.
            core_hz: Core clock at power-on in Hz, 0 for the description's reset clock.
            cpi: Core cycles per instruction, above 0.
            trace_capacity: Records the always-on trace ring keeps, above 0.
            spin_threshold: Iterations of a loop making no progress before a SPIN stop, 0 off.
            strict_peripherals: Whether an unmodelled register access raises ModelError.
            vector_table: Address of the vector table reset reads, None for the default.

        Raises:
            ConfigError: No arch module implements the core, or an option is out of range.
            ImageError: An image does not fit the memory map.
        """
        if not isinstance(description, MachineDescription):
            description = load_description(description)
        config = _core.MachineConfig()
        config.core_hz = core_hz
        config.cpi = cpi
        config.trace_capacity = trace_capacity
        config.spin_threshold = spin_threshold
        config.strict_peripherals = strict_peripherals
        config.vector_table = vector_table
        self._description = description
        self._handle = _core.MachineHandle(description, config)
        self._images: list[Image] = []
        for one in (image,) if isinstance(image, Image) else image:
            self.load(one)

    @property
    def description(self) -> MachineDescription:
        """Description the machine was built from."""
        return self._description

    @property
    def images(self) -> tuple[Image, ...]:
        """Images loaded so far, in load order."""
        return tuple(self._images)

    def load(self, image: Image) -> None:
        """Load one more image over memory, such as an application after a bootloader.

        Raises:
            ImageError: A segment falls outside the description's memory regions.
        """
        self._handle.load(image.native)
        self._images.append(image)

    def reset(self, kind: ResetKind = ResetKind.POWER_ON) -> None:
        """Reset the machine: clock to 0, core from its vector table, peripherals to reset values.

        Memory is kept.
        """
        self._handle.reset(_core.ResetKind(kind.value))

    def locate(self, address: int) -> Location:
        """Symbol, offset, file and line of an address, the rule stops use."""
        return location_of(self._handle.locate(address))

    def __repr__(self) -> str:
        return f"Machine({self._description.part}, images={len(self._images)})"
