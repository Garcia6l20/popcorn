"""Which popcorn library each native file belongs to, and the include edges the libraries allow.

`python -m popcorn_build.layering` checks every `#include` under native/src and
native/include/popcorn, the `layering` stage of `pcons run check`. `misplaced_sources` is the
configure-time rule that every library, extension and tool source belongs to exactly one of
them.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path

AREA_LIBRARY: dict[str, str] = {
    "core": "core",
    "image": "core",
    "arch": "core",
    "periph": "core",
    "plugin": "core",
    "armv8m": "armv8m",
    "usb": "usb",
    "stm32": "stm32",
    "devices": "devices",
    "obs": "obs",
}

ALLOWED: dict[str, set[str]] = {
    "core": {"core", "image", "arch", "periph", "plugin"},
    "armv8m": {"core", "image", "arch", "periph", "plugin", "armv8m"},
    "usb": {"core", "periph", "usb"},
    "stm32": {"core", "periph", "plugin", "usb", "stm32"},
    "devices": {"core", "periph", "plugin", "devices"},
    "obs": {"core", "image", "arch", "periph", "obs"},
}

FRAMEWORK_HEADERS = {
    "model.hpp",
    "firewall.hpp",
    "lines.hpp",
    "register_file.hpp",
    "bus_reset.hpp",
    "core_hold.hpp",
    "reset_listener.hpp",
    "snapshot_listener.hpp",
    "spi.hpp",
    "i2c.hpp",
    "uart.hpp",
}

UNCHECKED = {"bind", "tools"}

_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def area_of(path: Path, native: Path) -> str | None:
    """The area of a file under @p native, None for bind and tools.

    @p native is the repository's native directory. The include tree maps by its directory
    under popcorn/, the source tree by its directory under src/, the periph framework by its
    file name.
    """
    relative = path.relative_to(native)
    parts = relative.parts
    in_include = parts[0] == "include"
    inside = parts[2:] if in_include else parts[1:]
    if not inside:
        raise ValueError(f"{relative}: not mapped to a library")
    head, *rest = inside
    if in_include:
        if inside == ("export.hpp",):
            return "core"
        if head == "periph" and len(rest) == 1:
            if rest[0].startswith("usb"):
                return "usb"
            if rest[0] in FRAMEWORK_HEADERS:
                return "periph"
            raise ValueError(f"{relative}: not mapped to a library")
        if inside == ("arch", "arch.hpp"):
            return "arch"
    elif head in UNCHECKED or inside == ("obs", "bind.cpp"):
        return None
    if head in ("periph", "arch") and len(rest) > 1:
        return rest[0]
    if head in AREA_LIBRARY:
        return head
    raise ValueError(f"{relative}: not mapped to a library")


def _native_files(native: Path) -> list[Path]:
    roots = (native / "src", native / "include" / "popcorn")
    return sorted(
        path for root in roots for pattern in ("*.cpp", "*.hpp") for path in root.rglob(pattern)
    )


def include_violations(native: Path) -> list[str]:
    """Every include under @p native that its library may not have, as `file:line: a includes b`."""
    native = native.resolve()
    found: list[str] = []
    include_root = native / "include"
    for source in _native_files(native):
        area = area_of(source, native)
        if area is None:
            continue
        library = AREA_LIBRARY[area]
        for number, line in enumerate(source.read_text().splitlines(), start=1):
            match = _INCLUDE.match(line)
            if match is None:
                continue
            kind, name = match.groups()
            if kind == "<":
                if not name.startswith("popcorn/"):
                    continue
                target = include_root / name
            else:
                target = (source.parent / name).resolve()
                if not target.exists() and (include_root / name).exists():
                    target = include_root / name
            if not target.is_relative_to(native):
                continue
            included = area_of(target, native)
            where = f"{source.relative_to(native.parent)}:{number}: {library} includes"
            if included is None or included not in ALLOWED[library]:
                found.append(f"{where} {included or target.relative_to(native).parts[1]}")
            elif target.relative_to(native).parts[0] == "src" and (
                AREA_LIBRARY[included] != library
            ):
                found.append(f"{where} a private header of {AREA_LIBRARY[included]}")
    return found


def misplaced_sources(native: Path, owners: Mapping[str, Iterable[Path]]) -> list[str]:
    """Every `.cpp` under @p native/src that is in no list of @p owners or in more than one.

    @p owners maps each library, extension or tool to its sources.
    """
    native = native.resolve()
    held: dict[Path, list[str]] = {}
    for owner, sources in owners.items():
        for source in sources:
            held.setdefault(Path(source).resolve(), []).append(owner)
    found: list[str] = []
    for source in sorted((native / "src").rglob("*.cpp")):
        names = held.get(source.resolve(), [])
        if len(names) != 1:
            where = ", ".join(names) if names else "no library"
            found.append(f"{source.relative_to(native.parent)}: in {where}")
    return found


def main(argv: Sequence[str] = ()) -> int:
    """Print every include violation under the native directory, 1 when there is one."""
    native = Path(argv[0]) if argv else Path(__file__).resolve().parent.parent / "native"
    found = include_violations(native)
    for violation in found:
        print(violation)
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
