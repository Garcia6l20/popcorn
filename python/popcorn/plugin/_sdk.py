"""The plugin SDK installed with popcorn under `popcorn/sdk/`, read from its `sdk.json`."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from popcorn._errors import ConfigError

MANIFEST = "popcorn-plugin.json"


def package_directory() -> Path:
    """The popcorn package directory, which holds the `libpopcorn_*.so` plugins link."""
    return Path(__file__).resolve().parents[1]


@dataclass(frozen=True, slots=True)
class Sdk:
    """The compiler and flags popcorn was built with, and where the SDK files are.

    Attributes:
        root: The SDK directory, `popcorn/sdk`.
        version: The popcorn version the SDK belongs to.
        compiler: Absolute path of the C++ compiler.
        compiler_version: Its `-dumpfullversion`.
        compiler_ident: Its `__VERSION__`.
        cxx_flags: Compile flags.
        defines: Preprocessor definitions, without `-D`.
        link_flags: Link flags.
        ext_suffix: The extension suffix of a bindings module.
    """

    root: Path
    version: str
    compiler: str
    compiler_version: str
    compiler_ident: str
    cxx_flags: tuple[str, ...]
    defines: tuple[str, ...]
    link_flags: tuple[str, ...]
    ext_suffix: str

    @property
    def include(self) -> Path:
        """The SDK's include directory."""
        return self.root / "include"

    @property
    def runtime(self) -> Path:
        """The nanobind runtime archive a bindings module links."""
        return self.root / "nanobind" / "libnanobind-runtime.a"


def _strings(values: object, key: str, path: Path) -> tuple[str, ...]:
    items = cast(list[object], values) if isinstance(values, list) else None
    if items is None or not all(isinstance(value, str) for value in items):
        raise ConfigError(
            f"{path}: {key} is not a list of strings",
            file=str(path),
            key=key,
            expected="a list of strings",
        )
    return tuple(str(value) for value in items)


def sdk(path: Path | None = None) -> Sdk:
    """The SDK described by @p path, by default the `sdk.json` of this popcorn package.

    Raises:
        ConfigError: the file is missing or malformed, a popcorn built without the SDK.
    """
    path = path or package_directory() / "sdk" / "sdk.json"
    try:
        data: object = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise ConfigError(
            f"{path}: no plugin SDK, {error}",
            file=str(path),
            key="sdk",
            expected="the sdk.json of a popcorn built with its plugin SDK",
        ) from error
    if not isinstance(data, dict):
        raise ConfigError(
            f"{path}: not a JSON object", file=str(path), key="sdk", expected="a JSON object"
        )
    fields = cast(dict[str, object], data)
    text: dict[str, str] = {}
    for key in ("popcorn", "compiler", "compiler_version", "compiler_ident", "ext_suffix"):
        value = fields.get(key)
        if not isinstance(value, str):
            raise ConfigError(
                f"{path}: {key} is not a string", file=str(path), key=key, expected="a string"
            )
        text[key] = value
    return Sdk(
        root=path.parent,
        version=text["popcorn"],
        compiler=text["compiler"],
        compiler_version=text["compiler_version"],
        compiler_ident=text["compiler_ident"],
        cxx_flags=_strings(fields.get("cxx_flags"), "cxx_flags", path),
        defines=_strings(fields.get("defines"), "defines", path),
        link_flags=_strings(fields.get("link_flags"), "link_flags", path),
        ext_suffix=text["ext_suffix"],
    )
