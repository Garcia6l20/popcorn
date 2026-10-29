"""The freshness check shared by `fresh_u5` and `fresh_h5`: each shipped part against a fresh
ship build, when STM32_open_pin_data is installed. A builder, patch or facts change that changes
a shipped part's content fails there until `python -m popcorn.stm32.description ship` reruns."""

import json
from typing import cast

import popcorn
import pytest
from popcorn.stm32.description import _shipped
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._shipped import compose_shipped, shipped_names
from popcorn.stm32.description._sources import compute_source_digests, pin_data_sources

from .conftest import require_pin_data

U5_PREFIX = "STM32U5"
H5_PREFIX = "STM32H5"

EXTERNAL_SOURCES = ("svd", "mcu_db", "cmsis_header", "gpio_modes")
"""Source digests of files popcorn does not own. A difference there means another ST install,
not a stale shipped file."""


def names_of(prefix: str) -> list[str]:
    """The shipped parts whose name starts with @p prefix."""
    return [name for name in shipped_names() if name.startswith(prefix)]


def sources_of(name: str) -> dict[str, str]:
    """The source digests the shipped description of @p name records."""
    data = cast(dict[str, object], json.loads(compose_shipped(name)))
    return cast(dict[str, str], data["sources"])


def check_fresh(name: str) -> None:
    """Assert the shipped description of @p name equals a fresh ship build, skipping when the pin
    data or its sources are absent or another ST install."""
    root = require_pin_data()
    part, _, core = name.partition("-")
    try:
        sources = pin_data_sources(part, pin_data=root, core=core or None)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    local = compute_source_digests(sources)
    shipped_sources = sources_of(name)
    stale = [
        key for key in local if key.startswith("patch:") and local[key] != shipped_sources.get(key)
    ]
    assert not stale, f"{name}: shipped patch digests differ, rerun ship: {stale}"
    assert {k for k in shipped_sources if k.startswith("patch:")} == {
        k for k in local if k.startswith("patch:")
    }
    differing = [key for key in EXTERNAL_SOURCES if local.get(key) != shipped_sources.get(key)]
    if differing:
        pytest.skip(f"{name}: local {', '.join(differing)} differ from the shipped file's")
    fresh = popcorn.parse_description(_shipped.build_text(name, pin_data=root), "fresh")
    shipped = popcorn.parse_description(compose_shipped(name), name)
    assert popcorn.description_digest(fresh) == popcorn.description_digest(shipped)
