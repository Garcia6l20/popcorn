"""The plugin SDK under `popcorn/sdk/`: headers, the nanobind runtime archive and `sdk.json`.

`popcorn.plugin.build.PluginProject` compiles a plugin with the compiler and flags `sdk.json`
names, against `popcorn/sdk/include`, and links the `libpopcorn_*.so` of the package directory.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

from popcorn_build.sdk import reflex_include_dirs, sdk_inputs

SDK = "popcorn/sdk"
_VERSION_MACRO = re.compile(r'^#define __VERSION__ "(.*)"$', re.MULTILINE)


def _tree(root: Path) -> list[tuple[str, Path]]:
    """Every file under @p root, keyed by its path relative to @p root."""
    return [
        (path.relative_to(root).as_posix(), path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    ]


def sdk_headers(
    native_include: Path, reflex_root: Path, nanobind: Path, robin_map: Path
) -> list[tuple[str, Path]]:
    """The SDK's include tree, as (path under `include/`, source file), sorted by path.

    The public popcorn headers and the reflex headers they reach, the files the `;sdk` digest
    covers, plus the nanobind and robin-map headers a plugin with bindings compiles against.
    """
    reflex_dirs = reflex_include_dirs(reflex_root)
    headers: dict[str, Path] = {}
    for path in sdk_inputs(native_include, reflex_dirs):
        resolved = path.resolve()
        root = next(
            (
                directory
                for directory in (native_include, *reflex_dirs)
                if resolved.is_relative_to(directory.resolve())
            ),
            None,
        )
        if root is None:
            raise SystemExit(f"popcorn: SDK header {path} is under no include directory")
        headers[resolved.relative_to(root.resolve()).as_posix()] = path
    for root in (nanobind / "include", robin_map / "include"):
        headers.update(_tree(root))
    return sorted(headers.items())


def compiler_identity(compiler: str) -> tuple[str, str]:
    """`-dumpfullversion` and the `__VERSION__` macro of @p compiler, empty when it cannot run."""
    try:
        version = subprocess.run(
            [compiler, "-dumpfullversion"], capture_output=True, text=True, check=False
        ).stdout.strip()
        macros = subprocess.run(
            [compiler, "-dM", "-E", "-x", "c++", "/dev/null"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
    except OSError:
        return "", ""
    found = _VERSION_MACRO.search(macros)
    return version, found.group(1) if found else ""


def sdk_json(
    version: str,
    variant: str,
    compiler: str,
    cxx_flags: Sequence[str],
    defines: Sequence[str],
    link_flags: Sequence[str],
    ext_suffix: str,
) -> str:
    """The text of `sdk.json`, with the compiler's identity queried from @p compiler."""
    compiler_version, compiler_ident = compiler_identity(compiler)
    return (
        json.dumps(
            {
                "popcorn": version,
                "variant": variant,
                "compiler": compiler,
                "compiler_version": compiler_version,
                "compiler_ident": compiler_ident,
                "cxx_flags": list(cxx_flags),
                "defines": list(defines),
                "link_flags": list(link_flags),
                "ext_suffix": ext_suffix,
            },
            indent=2,
        )
        + "\n"
    )


def main(argv: Sequence[str]) -> int:
    """Write sdk.json, the build edge behind it.

    Arguments: output, then a JSON object with `version`, `variant`, `compiler`, `cxx_flags`,
    `defines`, `link_flags` and `ext_suffix`. The file is rewritten only when its text changes.
    """
    output, fields = argv
    values = json.loads(fields)
    text = sdk_json(
        values["version"],
        values["variant"],
        values["compiler"],
        values["cxx_flags"],
        values["defines"],
        values["link_flags"],
        values["ext_suffix"],
    )
    target = Path(output)
    if not target.is_file() or target.read_text() != text:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
