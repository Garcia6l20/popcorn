"""What popcorn models of an STM32 part, read from the native model registry."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

if TYPE_CHECKING:
    from popcorn._part_description import PartDescription


@dataclass(frozen=True)
class PeripheralSupport:
    """The model a machine built now gets for one peripheral instance.

    Attributes:
        name: Instance name, such as `USART1`.
        ip: IP name, as the description spells it.
        ip_version: IP version, as the description spells it.
        model: `"builtin"` for a built-in model, `"python"` for a class that
            `popcorn.model.register()` routes the IP to, None for no model.
    """

    name: str
    ip: str
    ip_version: str
    model: Literal["builtin", "python"] | None


def shipped_parts() -> list[str]:
    """The parts whose description ships with popcorn, `<RefName>[-<core>]`, sorted.

    `mcu()` loads each of them without ST's files.
    """
    from popcorn.stm32.description._shipped import shipped_names

    return shipped_names()


def support(description: PartDescription, *, trustzone: bool = False) -> list[PeripheralSupport]:
    """The model of each peripheral instance of a part, in description order, aliases left out.

    It asks the native registry, as a machine built now with no board model would: the latest
    registration of the IP whose glob matches the IP version serves it. A built-in model whose
    factory declines an instance, such as the GPIO model for LPGPIO1, counts as no model. Board
    models and plugin models are set per machine and do not show here.

    Args:
        description: The part, such as `mcu("STM32U575QGIx")`.
        trustzone: Whether the machine boots with TZEN = 1. The GTZC models serve only then.

    Raises:
        ConfigError: No arch module implements the part's core, or `trustzone` is set for a part
            without a security extension.
    """
    from popcorn import _core
    from popcorn._model_build import registered_classes

    builtin = set(_core.builtin_models(description, trustzone))
    found: list[PeripheralSupport] = []
    for peripheral in description.peripherals:
        if peripheral.alias_of is not None:
            continue
        glob = _core.registered_model_glob(peripheral.ip, peripheral.ip_version)
        model: Literal["builtin", "python"] | None = None
        if glob is not None and (peripheral.ip, glob) in registered_classes:
            model = "python"
        elif glob is not None and peripheral.name in builtin:
            model = "builtin"
        found.append(
            PeripheralSupport(peripheral.name, peripheral.ip, peripheral.ip_version, model)
        )
    return found
