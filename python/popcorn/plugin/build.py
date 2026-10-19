"""The pcons helper a plugin's `popcorn-plugin.py` uses.

```python
from popcorn.plugin.build import PluginProject

project = PluginProject("my_plugin")
project.native(["my_spi.cpp"])
project.bindings(["bind.cpp"])
project.wheel()
```

The environment comes from the SDK's `sdk.json` only: the compiler, flags and defines popcorn
was built with. The user's `CXXFLAGS`, `pyproject.toml` or pcons configuration are never read,
except the `[project] dependencies` of `pyproject.toml` that `wheel()` checks.

pcons's build backend, `pcons.pyproject`, runs the project's `pcons-build.py`, so a packaged
plugin's script carries that name. A dev-mode build runs `popcorn-plugin.py`.
"""

from __future__ import annotations

import contextlib
import json
import re
import sysconfig
import tomllib
from collections.abc import Callable, Sequence
from importlib import import_module
from pathlib import Path
from typing import cast

from pcons import (  # pyright: ignore[reportMissingTypeStubs]
    Project,
    Target,
    find_c_toolchain,
)

from popcorn._errors import ConfigError
from popcorn.plugin._build_cache import ignored
from popcorn.plugin._sdk import MANIFEST, Sdk, package_directory, sdk

get_var: Callable[[str, bool], bool] = import_module("pcons").get_var

_REQUIREMENT = re.compile(r"\s*([A-Za-z0-9][A-Za-z0-9._-]*)\s*(\[[^\]]*\])?(.*)", re.DOTALL)


def _without_build_rpath(target: Target) -> None:
    """Keep pcons from adding an rpath to the build's own shared libraries to @p target, on a
    pcons that adds one: the installed files carry only the rpaths set here. An older pcons,
    which adds none, refuses the option."""
    with contextlib.suppress(ValueError):
        target.set_option("build_rpath", False)


def _exact_pin(requirement: str, version: str) -> bool | None:
    """Whether @p requirement pins popcorn to exactly @p version, None when it is not popcorn's."""
    match = _REQUIREMENT.fullmatch(requirement)
    if match is None or re.sub(r"[-_.]+", "-", match[1]).lower() != "popcorn":
        return None
    return re.fullmatch(rf"\s*==\s*{re.escape(version)}\s*", match[3]) is not None


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
        self._wheel = get_var("PCONS_BUILD_WHEEL", False)
        self._native: Target | None = None
        self._bindings: Target | None = None
        self._wheeled = False

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
        visibility, so the bindings module links the classes it binds. In a dev-mode build the
        library's rpath is the popcorn package directory, absolute: the build never leaves this
        machine. In a wheel build it is `$ORIGIN/../popcorn`, the popcorn package beside the
        plugin's in site-packages.
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
        rpath = "$$ORIGIN/../popcorn" if self._wheel else str(self._package)
        library.private.link_flags.extend([f"-Wl,-rpath,{rpath}", "-Wl,-z,defs"])
        self._native = library
        self._write_manifest()
        return library

    def bindings(self, sources: Sequence[str | Path]) -> Target:
        """The bindings module `<name>/_native`, an extension linking the plugin library.

        Its sources include `popcorn/plugin/bindings.hpp`, define the module with
        `NB_MODULE(_native, m)` and call `popcorn::plugin::require_core(m)` first. Import it with
        `Plugin.module()`, never directly. Its rpaths are `$ORIGIN/..` and the popcorn package
        directory in a dev-mode build, `$ORIGIN` and `$ORIGIN/../popcorn` in a wheel build,
        where the plugin library sits beside it.

        Raises:
            ValueError: `native()` was not called first, or `bindings()` is called twice or
                after `wheel()`.
        """
        if self._native is None:
            raise ValueError(f"plugin {self.name}: native() comes before bindings()")
        if self._bindings is not None:
            raise ValueError(f"plugin {self.name}: bindings() is called once")
        if self._wheeled:
            raise ValueError(f"plugin {self.name}: bindings() comes before wheel()")
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
        rpaths = (
            ["$$ORIGIN", "$$ORIGIN/../popcorn"]
            if self._wheel
            else ["$$ORIGIN/..", str(self._package)]
        )
        module.private.link_flags.extend(f"-Wl,-rpath,{rpath}" for rpath in rpaths)
        module.link(self._native)
        _without_build_rpath(module)
        module.private.link_dirs.append(str(self._package))
        module.private.link_libs.append("popcorn_core")
        self._bindings = module
        self._write_manifest()
        return module

    def wheel(self) -> None:
        """The wheel contents for pcons's build backend: package `<name>`, holding the plugin
        library, the bindings module and every file of the project's `<name>/` directory.

        Nothing is installed outside a wheel build, so a dev-mode script may call it too. The
        backend writes `Requires-Dist` from `pyproject.toml` alone, so the project's
        `[project] dependencies` must already pin `popcorn==<version>` exactly, the version of
        this SDK: the loader refuses a plugin built for any other popcorn. A plugin wheel needs
        a bindings module: the backend refuses a wheel with no extension module.

        Raises:
            ConfigError: in a wheel build, `bindings()` was not called, `pyproject.toml` lacks
                the exact pin, or the project has no `<name>/__init__.py`.
            ValueError: `native()` was not called first, or `wheel()` is called twice.
        """
        if self._native is None:
            raise ValueError(f"plugin {self.name}: native() comes before wheel()")
        if self._wheeled:
            raise ValueError(f"plugin {self.name}: wheel() is called once")
        if not self._wheel:
            self._wheeled = True
            return
        root = Path(self._project.root_dir)
        if self._bindings is None:
            raise ConfigError(
                f"plugin {self.name}: a plugin wheel needs bindings() before wheel(), pcons's"
                " build backend refuses a wheel with no extension module",
                file=str(root / "pcons-build.py"),
                key="bindings",
                expected="a pcons-build.py calling PluginProject.bindings() before wheel()",
            )
        self._check_pin(root / "pyproject.toml")
        package = root / self.name
        if not (package / "__init__.py").is_file():
            raise ConfigError(
                f"plugin {self.name}: no package {package}",
                file=str(package / "__init__.py"),
                key="package",
                expected=f"a package {self.name}/ whose __init__.py defines the installed plugin",
            )
        installs = [
            self._project.Install(
                self.name, [self._native, self._bindings], name=f"install-{self.name}"
            )
        ]
        for path in sorted(package.rglob("*")):
            relative = path.relative_to(package)
            if not path.is_file() or any(ignored(part) for part in relative.parts):
                continue
            installs.append(
                self._project.InstallAs(
                    f"{self.name}/{relative.as_posix()}",
                    path,
                    name=f"install-{self.name}-{relative.as_posix().replace('/', '-')}",
                )
            )
        self._project.Alias("wheel", *installs)
        self._wheeled = True

    def _check_pin(self, pyproject: Path) -> None:
        pin = f"popcorn=={self._sdk.version}"
        try:
            project: object = tomllib.loads(pyproject.read_text()).get("project", {})
        except (OSError, tomllib.TOMLDecodeError) as error:
            raise ConfigError(
                f"{pyproject}: {error}", file=str(pyproject), key="pyproject", expected=pin
            ) from error
        dependencies: object = (
            cast(dict[str, object], project).get("dependencies", [])
            if isinstance(project, dict)
            else []
        )
        listed = (
            [str(item) for item in cast(list[object], dependencies)]
            if isinstance(dependencies, list)
            else []
        )
        pins = [
            (requirement, exact)
            for requirement in listed
            if (exact := _exact_pin(requirement, self._sdk.version)) is not None
        ]
        if pins and all(exact for _, exact in pins):
            return
        found = ", ".join(repr(requirement) for requirement, _ in pins) or "no popcorn requirement"
        raise ConfigError(
            f"{pyproject}: a plugin wheel pins {pin} exactly in [project] dependencies, the"
            f" loader refuses a plugin built for any other popcorn, found {found}",
            file=str(pyproject),
            key="project.dependencies",
            expected=pin,
        )
