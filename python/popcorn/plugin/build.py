"""The pcons helper a plugin's `popcorn-plugin.py` uses.

```python
from popcorn.plugin.build import PluginProject

project = PluginProject("my_plugin")
project.native(["my_spi.cpp"])
project.bindings(["bind.cpp"])
```

The environment comes from the SDK's `sdk.json` only: the compiler, flags and defines popcorn
was built with. The user's `CXXFLAGS`, `pyproject.toml` or pcons configuration are never read.
"""

from __future__ import annotations

import json
import sysconfig
from collections.abc import Sequence
from pathlib import Path

from pcons import Project, Target, find_c_toolchain  # pyright: ignore[reportMissingTypeStubs]

from popcorn.plugin._sdk import MANIFEST, Sdk, package_directory, sdk


class PluginProject:
    """A popcorn plugin's pcons project, built with popcorn's compiler and flags.

    Attributes:
        name: The plugin's name, the stem of its library `lib<name>.so` and the package of its
            bindings module `<name>/_native`.
    """

    def __init__(self, name: str) -> None:
        if not name.isidentifier():
            raise ValueError(f"a plugin name is a Python identifier, got {name!r}")
        self.name = name
        self._sdk: Sdk = sdk()
        self._package = package_directory()
        self._project = Project(name)
        self._env = self._project.Environment(toolchain=find_c_toolchain(prefer=["gcc"]))
        self._env.cxx.cmd = self._sdk.compiler
        self._env.link.cmd = self._sdk.compiler
        self._env.cxx.flags.extend(self._sdk.cxx_flags)
        self._env.cxx.defines.extend(self._sdk.defines)
        self._env.link.flags.extend(self._sdk.link_flags)
        self._native: Target | None = None
        self._bindings: Target | None = None

    @property
    def _build_dir(self) -> Path:
        return Path(self._project.build_dir)

    def _write_manifest(self) -> None:
        manifest = {
            "name": self.name,
            "library": f"lib{self.name}.so",
            "bindings": (
                None if self._bindings is None else f"{self.name}/_native{self._sdk.ext_suffix}"
            ),
        }
        path = self._build_dir / MANIFEST
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(manifest, indent=2) + "\n")

    def _compiles_against_sdk(self, target: Target) -> None:
        target.private.include_dirs.append(str(self._sdk.include))

    def native(self, sources: Sequence[str | Path]) -> Target:
        """The plugin library `lib<name>.so`, linked to `libpopcorn_core.so`.

        Its sources define the entry point with `POPCORN_PLUGIN`. Symbols keep the default
        visibility, so the bindings module links the classes it binds. The library's rpath is the
        popcorn package directory, absolute: a dev-mode build never leaves this machine.
        """
        if self._native is not None:
            raise ValueError(f"plugin {self.name}: native() is called once")
        library = self._project.SharedLibrary(
            f"lib{self.name}", self._env, sources=[str(source) for source in sources]
        )
        library.output_name = self.name
        library.output_suffix = ".so"
        self._compiles_against_sdk(library)
        library.private.link_dirs.append(str(self._package))
        library.private.link_libs.append("popcorn_core")
        library.private.link_flags.extend([f"-Wl,-rpath,{self._package}", "-Wl,-z,defs"])
        self._native = library
        self._write_manifest()
        return library

    def bindings(self, sources: Sequence[str | Path]) -> Target:
        """The bindings module `<name>/_native`, an extension linking the plugin library.

        Its sources include `popcorn/plugin/bindings.hpp`, define the module with
        `NB_MODULE(_native, m)` and call `popcorn::plugin::require_core(m)` first. Import it with
        `Plugin.module()`, never directly.
        """
        if self._native is None:
            raise ValueError(f"plugin {self.name}: native() comes before bindings()")
        if self._bindings is not None:
            raise ValueError(f"plugin {self.name}: bindings() is called once")
        module = self._project.SharedLibrary(
            f"{self.name}._native",
            self._env,
            sources=[*(str(source) for source in sources), str(self._sdk.runtime)],
        )
        module.output_name = "_native"
        module.output_prefix = f"{self.name}/"
        module.output_suffix = self._sdk.ext_suffix
        self._compiles_against_sdk(module)
        module.private.compile_flags.append("-fvisibility=hidden")
        module.private.system_include_dirs.append(sysconfig.get_paths()["include"])
        module.private.link_flags.extend(["-Wl,-rpath,$$ORIGIN/..", f"-Wl,-rpath,{self._package}"])
        module.link(self._native)
        module.private.link_dirs.append(str(self._package))
        module.private.link_libs.append("popcorn_core")
        self._bindings = module
        self._write_manifest()
        return module
