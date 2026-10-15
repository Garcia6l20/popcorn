"""Native popcorn plugins: C++ models and devices built against popcorn's C++ API.

A dev-mode plugin is a source directory holding `popcorn-plugin.py`, a pcons script using
`popcorn.plugin.build.PluginProject`. It builds on first use into a content-addressed cache,
`$POPCORN_PLUGIN_CACHE`, else `$XDG_CACHE_HOME/popcorn/plugins`, else
`~/.cache/popcorn/plugins`. Building needs pcons, ninja and the GCC build popcorn was built with.
The `popcorn[plugin]` extra installs pcons and ninja.

```python
PLUGIN = Plugin.dev(Path(__file__).parent / "plugin")
EchoFlash = PLUGIN.module().EchoFlashBase
```

A packaged plugin is a wheel pinning `popcorn==<version>` exactly, whose package defines an
installed plugin and names it in the `popcorn.plugins` entry point group:

```python
PLUGIN = Plugin.installed(
    "my_plugin", Path(__file__).parent / "libmy_plugin.so", bindings="my_plugin._native"
)
```

`find("my_plugin")` returns it. Its device classes come from `PLUGIN.module()`, never from
`import my_plugin._native`: a direct import maps the library before the loader checks it, and
`MachineDescription` refuses a device whose class a nanobind module bound unless that module is
popcorn's or one `module()` imported.

A loaded plugin is never unloaded: a process that changed a dev plugin's sources after building
it raises ConfigError, restart it to load the edit.
"""

from __future__ import annotations

import importlib
import importlib.machinery
import importlib.metadata
import importlib.util
import inspect
import sys
import types
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from popcorn._errors import ConfigError

from . import _build_cache, _records

__all__ = ["Plugin", "PluginModel", "find"]

_ENTRY_POINTS = "popcorn.plugins"


@dataclass(frozen=True, slots=True)
class Plugin:
    """A native plugin: a dev-mode source directory or an installed library.

    Attributes:
        name: The plugin's name, as its `POPCORN_PLUGIN` entry spells it.
        source: The dev-mode source directory, None for an installed plugin.
        library: The installed library, None for a dev-mode plugin.
        bindings: The dotted name of an installed plugin's bindings module, None when it has
            none and for a dev-mode plugin.
    """

    name: str
    source: Path | None = None
    library: Path | None = None
    bindings: str | None = None

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
        bindings = cast(object, self.bindings)
        if bindings is None:
            return
        if self.source is not None:
            raise ConfigError(
                f"plugin {self.name}: bindings {bindings!r} on a dev-mode plugin",
                key="bindings",
                expected="no bindings name, a dev-mode plugin's bindings module comes from its"
                " build",
            )
        if not isinstance(bindings, str) or not all(
            part.isidentifier() for part in bindings.split(".")
        ):
            raise ConfigError(
                f"plugin {self.name}: bindings {bindings!r} is not a dotted module name",
                key="bindings",
                expected="the dotted name of the plugin's bindings module",
            )
        if bindings.partition(".")[0] in ("popcorn", _records.MODULE_PREFIX):
            raise ConfigError(
                f"plugin {self.name}: bindings {bindings!r} is under popcorn's own packages",
                key="bindings",
                expected=f"a bindings module outside popcorn and {_records.MODULE_PREFIX}",
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

    @classmethod
    def installed(cls, name: str, library: Path | str, bindings: str | None = None) -> Plugin:
        """The installed plugin @p name whose library is @p library, never built.

        A relative @p library is taken from the working directory. @p bindings is the dotted
        name of its bindings extension module, such as `my_plugin._native`, None for a plugin
        without one. `module()` refuses a name that imports a Python module.
        A wheel's package defines it and names it in the `popcorn.plugins` entry point group,
        `find` returns it.

        Raises:
            ConfigError: @p name is not a Python identifier, or @p bindings is not a dotted
                module name or is under `popcorn` or `popcorn_plugins`.
        """
        return cls(name=name, library=Path(library).absolute(), bindings=bindings)

    def model(self, name: str) -> PluginModel:
        """The model this plugin registers under @p name, for `Periph(model=)`.

        Nothing builds or loads here: a machine description naming it can be declared before
        the plugin builds. `MachineDescription.machine` checks the name against the models the
        loaded plugin registers.

        Raises:
            ConfigError: @p name is not a non-empty str.
        """
        return PluginModel(self, name)

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
        is refused before any of its static constructors run. A dev-mode module is imported as
        `popcorn_plugins.<name>._native`, an installed one under its `bindings` name. A second
        call returns the same module. This is the only supported import of a bindings module.

        Raises:
            ConfigError: the build, the load or the import is refused, the plugin has no
                bindings module, an installed plugin's bindings is not an extension module,
                its `POPCORN_PLUGIN` name differs from `name`, or another plugin of this name
                was imported in this process.
        """
        module_name = (
            f"{_records.MODULE_PREFIX}.{self.name}._native"
            if self.source is not None or self.bindings is None
            else self.bindings
        )
        if (recorded := _records.imported(module_name)) is not None:
            if recorded[0] != self:
                raise ConfigError(
                    f"plugin {self.name}: {module_name} was imported for {recorded[0]}",
                    key="name",
                    expected="one plugin per name in a process",
                )
            return recorded[1]
        if self.source is None:
            return self._installed_module()
        built = _build_cache.build(self.name, self.source)
        if built.bindings is None:
            raise ConfigError(
                f"plugin {self.name} has no bindings module",
                file=str(self.source),
                key="bindings",
                expected="a plugin whose popcorn-plugin.py calls PluginProject.bindings()",
            )
        _records.load(self, built.library)
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

    def _installed_module(self) -> types.ModuleType:
        if self.bindings is None:
            raise ConfigError(
                f"plugin {self.name} has no bindings module",
                file=str(self.library),
                key="bindings",
                expected="an installed plugin given its bindings module name",
            )
        assert self.library is not None
        _records.load(self, self.library)
        try:
            module = importlib.import_module(self.bindings)
        except ImportError as error:
            raise ConfigError(
                f"plugin {self.name}: bindings module {self.bindings} does not import, {error}",
                file=str(self.library),
                key="bindings",
                expected="the bindings module installed beside the plugin library",
            ) from error
        spec = module.__spec__
        if spec is None or not isinstance(spec.loader, importlib.machinery.ExtensionFileLoader):
            raise ConfigError(
                f"plugin {self.name}: bindings module {self.bindings} is not an extension module",
                file=str(self.library),
                key="bindings",
                expected="the name of the plugin's extension module, which the digest hashes",
            )
        _records.note_imported(self.bindings, self, module)
        return module


@dataclass(frozen=True, slots=True)
class PluginModel:
    """One named model of a plugin, for `Periph(model=)`: `PLUGIN.model("rng")`.

    Attributes:
        plugin: The plugin, which the machine description must list in `plugins`.
        name: The name the plugin's init registers the model under.

    Raises:
        ConfigError: `plugin` is not a `Plugin`, key `plugin`, or `name` is not a non-empty
            `str`, key `model`.
    """

    plugin: Plugin
    name: str

    def __post_init__(self) -> None:
        plugin = cast(object, self.plugin)
        if not isinstance(plugin, Plugin):
            raise ConfigError(
                f"plugin model {self.name!r}: plugin {plugin!r} is not a Plugin",
                key="plugin",
                expected="a popcorn.plugin.Plugin",
            )
        name = cast(object, self.name)
        if not isinstance(name, str) or not name:
            raise ConfigError(
                f"plugin {self.plugin.name}: model name {name!r} is not a non-empty str",
                key="model",
                expected="the name of a model the plugin registers",
            )


def find(name: str) -> Plugin:
    """The installed plugin the `popcorn.plugins` entry point @p name names.

    The entry point's value is the `Plugin` a wheel's package defines, such as
    `my_plugin = "my_plugin:PLUGIN"`. Importing it loads nothing native.

    Raises:
        ConfigError: no installed distribution provides @p name, the message listing the names
            installed, two distributions provide it, naming both, one declares it twice, its
            module does not import, or its value is not a `Plugin` named @p name.
    """
    if not isinstance(cast(object, name), str) or not name:
        raise ConfigError(
            f"plugin name {name!r} is not a non-empty str",
            key="name",
            expected="the name of an installed plugin",
        )
    points = importlib.metadata.entry_points(group=_ENTRY_POINTS)
    matches = [point for point in points if point.name == name]
    if not matches:
        installed = ", ".join(sorted({point.name for point in points})) or "none"
        raise ConfigError(
            f"no installed plugin {name}, installed: {installed}",
            key="name",
            expected=f"the name of a {_ENTRY_POINTS} entry point, one of {installed}",
        )
    if len(matches) > 1:
        names = sorted(
            {point.value if point.dist is None else point.dist.name for point in matches}
        )
        provided = (
            f"declared twice by {names[0]}"
            if len(names) == 1
            else f"provided by {' and '.join(names)}"
        )
        raise ConfigError(
            f"installed plugin {name} is {provided}",
            key="name",
            expected=f"one distribution providing the {_ENTRY_POINTS} entry point {name}",
        )
    try:
        value: object = matches[0].load()
    except (ImportError, AttributeError) as error:
        raise ConfigError(
            f"installed plugin {name}: {matches[0].value} does not load, {error}",
            key="name",
            expected="an entry point naming a popcorn.plugin.Plugin",
        ) from error
    if not isinstance(value, Plugin) or value.name != name:
        raise ConfigError(
            f"installed plugin {name}: {matches[0].value} is {value!r}",
            key="name",
            expected=f"a popcorn.plugin.Plugin named {name}",
        )
    return value
