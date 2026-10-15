"""Process-local records of dev plugins: the ones `Plugin.dev` made and the bindings modules
`Plugin.module()` imported."""

from __future__ import annotations

import types
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from . import Plugin

MODULE_PREFIX = "popcorn_plugins"

_constructed: list[tuple[Plugin, Path | None]] = []
_modules: dict[str, tuple[Plugin, types.ModuleType]] = {}


def note_constructed(plugin: Plugin, origin: Path | None) -> None:
    """Record that the file @p origin made @p plugin, once per distinct pair."""
    record = (plugin, origin)
    if record not in _constructed:
        _constructed.append(record)


def constructed() -> list[tuple[Plugin, Path | None]]:
    """Every dev plugin `Plugin.dev` made in this process, with the file that called it, once
    per distinct pair."""
    return list(_constructed)


def note_imported(module_name: str, plugin: Plugin, module: types.ModuleType) -> None:
    """Record that `module()` of @p plugin imported @p module as @p module_name."""
    _modules[module_name] = (plugin, module)


def imported(module_name: str) -> tuple[Plugin, types.ModuleType] | None:
    """The plugin whose `module()` imported @p module_name and the module, None when none did."""
    return _modules.get(module_name)


def recorded(module_name: str) -> Plugin | None:
    """The plugin whose `module()` imported @p module_name, None when none did."""
    found = _modules.get(module_name)
    return None if found is None else found[0]
