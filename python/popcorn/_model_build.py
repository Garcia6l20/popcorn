"""How a machine builds a Python model: the context the model's constructor attaches to."""

from __future__ import annotations

import contextvars
from collections.abc import Callable
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._errors import ConfigError

if TYPE_CHECKING:
    from popcorn.model import Model

building: contextvars.ContextVar[_core.ModelContext | None] = contextvars.ContextVar(
    "popcorn_model_building", default=None
)
"""Context of the model being built on this thread, None outside a build."""


def build_model(maker: Callable[[], Model], context: _core.ModelContext) -> Model:
    """Build a model for `context`'s peripheral, what the native factories call.

    Raises:
        ConfigError: `maker` returned something other than a `popcorn.model.Model`.
        TypeError: The model's constructor did not call `super().__init__()`.
    """
    from popcorn.model import Model

    token = building.set(context)
    try:
        made = cast(object, maker())
    finally:
        building.reset(token)
    name = context.description().name
    if not isinstance(made, Model):
        raise ConfigError(
            f"the model of {name} is a {type(made).__name__}, not a popcorn.model.Model",
            key=f"models.{name}",
            expected="a popcorn.model.Model",
        )
    made.capture_initial_state(context=context)
    return made
