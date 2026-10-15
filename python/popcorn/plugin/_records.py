"""Process-local records of plugins: the ones `Plugin.dev` made, the bindings modules
`Plugin.module()` imported, and the loaded libraries."""

from __future__ import annotations

import types
from pathlib import Path
from typing import TYPE_CHECKING

from popcorn import _core
from popcorn._errors import ConfigError

if TYPE_CHECKING:
    from . import Plugin

MODULE_PREFIX = "popcorn_plugins"

type Loaded = _core._Plugin  # pyright: ignore[reportPrivateUsage]
"""A loaded native plugin, as `_core._load_plugin` returns it."""

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
    """The plugin whose `module()` imported @p module_name or the module a submodule
    @p module_name belongs to, None when none did."""
    found = _modules.get(module_name)
    if found is not None:
        return found[0]
    for name, (plugin, _) in _modules.items():
        if module_name.startswith(f"{name}."):
            return plugin
    return None


def load(plugin: Plugin, library: Path) -> Loaded:
    """Load @p library, the library of @p plugin, once per library file, and check that its
    `POPCORN_PLUGIN` name is `plugin.name`.

    Raises:
        ConfigError: The loader refuses the library, or it names another plugin, key `name`.
    """
    loaded = _core._load_plugin(str(library))  # pyright: ignore[reportPrivateUsage]
    if loaded.name != plugin.name:
        raise ConfigError(
            f"plugin {plugin.name}: its library {library} is POPCORN_PLUGIN {loaded.name},"
            f" not {plugin.name}",
            file=str(library),
            key="name",
            expected=f"POPCORN_PLUGIN({plugin.name}, ...)",
        )
    return loaded
