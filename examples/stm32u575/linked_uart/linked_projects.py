"""The two projects of the linked_uart example, shared by the conftest.py files below."""

from pathlib import Path

import popcorn

HERE = Path(__file__).parent

CONTROLLER = popcorn.Project(
    name="linked-controller",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/linked_controller.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)

RESPONDER = popcorn.Project(
    name="linked-responder",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/linked_responder.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)
