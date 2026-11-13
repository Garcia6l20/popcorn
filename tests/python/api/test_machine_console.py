"""MachineConsole and MachineDescription.console: validation, digest, machine checks."""

from __future__ import annotations

import dataclasses
import weakref
from collections.abc import Sequence
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import _machine_description
from popcorn._rng import RngOptions
from popcorn.board import UartDevice
from popcorn.pytest._session import Key

from .conftest import collected

PLAIN_DIGEST = "c1fef57e3020d0713b9ead07c1fab2394e8855dce16a87439f02662500bcd068"
BOOT_AND_RNG_DIGEST = "ac342e38bf906641d4d60cdbc60f898dca9cc04bb6f2e8984255591fc224a6dc"


class Quiet(UartDevice):
    pass


def plain() -> popcorn.MachineDescription:
    return popcorn.MachineDescription(part="STM32U575QGIx", images=())


def test_the_defaults_print_nothing() -> None:
    console = popcorn.MachineConsole()
    assert console.uart == ()
    assert console.tail == 4096


@pytest.mark.parametrize(
    ("given", "held"),
    [("USART1", ("USART1",)), (["A", "B"], ("A", "B")), (("A", "B"), ("A", "B")), (None, ())],
)
def test_uart_is_held_as_a_tuple(given: str | Sequence[str] | None, held: tuple[str, ...]) -> None:
    assert popcorn.MachineConsole(uart=given).uart == held


@pytest.mark.parametrize(
    ("kwargs", "key"),
    [
        ({"tail": 0}, "console.tail"),
        ({"tail": -1}, "console.tail"),
        ({"tail": True}, "console.tail"),
        ({"tail": 1.5}, "console.tail"),
        ({"uart": ""}, "console.uart"),
        ({"uart": "US ART"}, "console.uart"),
        ({"uart": ("A", "A")}, "console.uart"),
        ({"uart": ("A", 3)}, "console.uart"),
        ({"uart": 3}, "console.uart"),
    ],
)
def test_a_bad_console_names_its_path(kwargs: dict[str, Any], key: str) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.MachineConsole(**kwargs)
    assert caught.value.key == key


def test_the_field_is_checked_and_carried() -> None:
    assert plain().console is None
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.MachineDescription(part="STM32U575QGIx", images=(), console=cast(Any, 3))
    assert caught.value.key == "console"
    console = popcorn.MachineConsole(uart="USART1")
    base = plain().replace(console=console)
    assert dataclasses.replace(base, name="other").console is console
    other = popcorn.MachineConsole(uart="USART2")
    varied = base.with_configs(own=base.replace(console=other))
    assert varied.console is console
    assert varied.config("own").console is other


def test_the_console_is_not_in_the_digest() -> None:
    assert plain().digest == PLAIN_DIGEST
    rng = RngOptions(seed=7)
    boot = popcorn.Boot(vtor=0x0800_0000, until="main")
    described = plain().replace(boot=boot, rng=rng)
    assert described.digest == BOOT_AND_RNG_DIGEST
    first = popcorn.MachineConsole(uart="USART1")
    second = popcorn.MachineConsole(uart=("USART1", "USART2"), tail=8)
    assert described.replace(console=first).digest == BOOT_AND_RNG_DIGEST
    assert plain().replace(console=first).digest == plain().replace(console=second).digest
    with_images = popcorn.MachineDescription(
        part="STM32U575QGIx", images=[popcorn.Bin(Path("/nonexistent/a.bin"), 0x0800_0000)]
    )
    assert with_images.replace(console=first).digest == with_images.digest


def test_two_descriptions_differing_in_console_share_one_key() -> None:
    first = plain().replace(console=popcorn.MachineConsole(uart="USART1"))
    second = plain().replace(console=popcorn.MachineConsole(uart="USART2"))
    assert Key(first.digest, False).identity == Key(second.digest, False).identity


def test_a_console_on_a_uart_of_the_part_builds(u575: popcorn.PartDescription) -> None:
    del u575
    description = plain().replace(
        console=popcorn.MachineConsole(uart=("USART1", "LPUART1", "SEC_USART2"))
    )
    assert isinstance(description.machine(), popcorn.Machine)


@pytest.mark.parametrize("name", ["USART99", "TIM2"])
def test_a_console_on_no_uart_is_refused_and_the_machine_is_freed_with_the_error(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch, name: str
) -> None:
    del u575
    built: list[weakref.ref[popcorn.Machine]] = []
    real = _machine_description.Machine

    def capture(*args: Any, **kwargs: Any) -> popcorn.Machine:
        machine = real(*args, **kwargs)
        built.append(weakref.ref(machine))
        return machine

    monkeypatch.setattr(_machine_description, "Machine", capture)
    description = plain().replace(console=popcorn.MachineConsole(uart=name))
    with pytest.raises(popcorn.ConfigError, match=rf"console\.uart.*USART1.*{name}") as caught:
        description.machine()
    assert caught.value.key == "console.uart"
    assert len(built) == 1
    del caught
    assert collected(built[0])


def test_an_alias_of_a_uart_with_a_device_is_refused(u575: popcorn.PartDescription) -> None:
    del u575
    description = plain().replace(
        board={"USART1": popcorn.Periph(devices=[popcorn.Device(Quiet)])},
        console=popcorn.MachineConsole(uart="SEC_USART1"),
    )
    with pytest.raises(popcorn.ConfigError, match=r"console\.uart.*not SEC_USART1"):
        description.machine()


def test_a_console_on_a_uart_with_a_device_is_refused(u575: popcorn.PartDescription) -> None:
    del u575
    description = plain().replace(
        board={"USART1": popcorn.Periph(devices=[popcorn.Device(Quiet)])},
        console=popcorn.MachineConsole(uart="USART1"),
    )
    with pytest.raises(popcorn.ConfigError, match=r"console\.uart.*not USART1"):
        description.machine()
