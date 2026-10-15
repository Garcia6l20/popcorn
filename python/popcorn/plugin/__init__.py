"""Native popcorn plugins: C++ models and devices built against popcorn's C++ API.

A dev-mode plugin is a source directory holding `popcorn-plugin.py`, a pcons script using
`popcorn.plugin.build.PluginProject`. It builds on first use into a content-addressed cache,
`$POPCORN_PLUGIN_CACHE`, else `$XDG_CACHE_HOME/popcorn/plugins`, else
`~/.cache/popcorn/plugins`. Building needs pcons, ninja and the GCC build popcorn was built with.

```python
PLUGIN = Plugin.dev(Path(__file__).parent / "plugin")
EchoFlash = PLUGIN.module().EchoFlashBase
```

A loaded plugin is never unloaded: a process that changed a dev plugin's sources after building
it raises ConfigError, restart it to load the edit.
"""

from __future__ import annotations

import importlib.util
import inspect
import sys
import types
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from popcorn import _core
from popcorn._errors import ConfigError

from . import _build_cache, _records

__all__ = ["Plugin"]


@dataclass(frozen=True, slots=True)
class Plugin:
    """A native plugin: a dev-mode source directory or an installed library.

    Attributes:
        name: The plugin's name, as its `POPCORN_PLUGIN` entry spells it.
        source: The dev-mode source directory, None for an installed plugin.
        library: The installed library, None for a dev-mode plugin.
    """

    name: str
    source: Path | None = None
    library: Path | None = None

    def __post_init__(self) -> None:
        if not isinstance(cast(object, self.name), str) or not self.name.isidentifier():
            raise ConfigError(
                f"plugin name {self.name!r} is not a Python identifier",
                key="name",
                expected="a Python identifier, the name POPCORN_PLUGIN gives",
            )
        if (self.source is None) == (self.library is None):
            raise ConfigError(
                f"plugin {self.name}: exactly one of source and library",
                key="source",
                expected="a source directory or an installed library",
            )

    @classmethod
    def dev(cls, source: Path | str, name: str | None = None) -> Plugin:
        """The dev-mode plugin in the directory @p source, named @p name or after the directory.

        It builds on first use, nothing builds here. Under pytest, a plugin constructed by a
        `conftest.py` is built when its directory is collected, and a build failure is that
        directory's collection error.
        """
        path = Path(source).resolve()
        plugin = cls(name=path.name if name is None else name, source=path)
        caller = inspect.currentframe()
        origin = caller.f_back.f_globals.get("__file__") if caller and caller.f_back else None
        where = Path(origin).resolve() if isinstance(origin, str) else None
        _records.note_constructed(plugin, where)
        return plugin

    def built(self) -> Path:
        """The library to load, building a dev-mode plugin first if its cache entry is missing.

        Raises:
            ConfigError: the build is refused or fails, see the module doc.
        """
        if self.library is not None:
            return self.library
        assert self.source is not None
        return _build_cache.build(self.name, self.source).library

    def module(self) -> types.ModuleType:
        """The plugin's bindings module, loaded after the plugin passed the loader's checks.

        The library loads first with `_core._load_plugin`, so a plugin built for another popcorn
        is refused before any of its static constructors run. The module is imported as
        `popcorn_plugins.<name>._native`, a second call returns the same module. This is the
        only supported import of a bindings module.

        Raises:
            ConfigError: the build or the load is refused, the plugin has no bindings module,
                its `POPCORN_PLUGIN` name differs from `name`, or another plugin of this name
                was imported in this process.
        """
        module_name = f"{_records.MODULE_PREFIX}.{self.name}._native"
        if (recorded := _records.imported(module_name)) is not None:
            if recorded[0] != self:
                raise ConfigError(
                    f"plugin {self.name}: {module_name} was imported for {recorded[0]}",
                    key="name",
                    expected="one plugin per name in a process",
                )
            return recorded[1]
        if self.source is None:
            raise ConfigError(
                f"plugin {self.name}: a plugin given by its library has no bindings module to"
                " import",
                key="bindings",
                expected="a dev-mode plugin",
            )
        built = _build_cache.build(self.name, self.source)
        if built.bindings is None:
            raise ConfigError(
                f"plugin {self.name} has no bindings module",
                file=str(self.source),
                key="bindings",
                expected="a plugin whose popcorn-plugin.py calls PluginProject.bindings()",
            )
        loaded = _core._load_plugin(str(built.library))  # pyright: ignore[reportPrivateUsage]
        if loaded.name != self.name:
            raise ConfigError(
                f"plugin {self.name}: its library {built.library} is POPCORN_PLUGIN"
                f" {loaded.name}, not {self.name}",
                file=str(built.library),
                key="name",
                expected=f"POPCORN_PLUGIN({self.name}, ...)",
            )
        spec = importlib.util.spec_from_file_location(module_name, built.bindings)
        if spec is None or spec.loader is None:
            raise ConfigError(
                f"plugin {self.name}: {built.bindings} is not an extension module",
                file=str(built.bindings),
                key="bindings",
                expected="a Python extension module",
            )
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            del sys.modules[module_name]
            raise
        _records.note_imported(module_name, self, module)
        return module
