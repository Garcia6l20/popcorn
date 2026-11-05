"""Resolution goldens of the STM32 model rows, and the diff between two golden trees.

python -m popcorn_build.resolution_diff update [DIR]   write the goldens of every family
python -m popcorn_build.resolution_diff diff OLD NEW [--allow COLUMN ...]   print the per-column
changes, exit 1 when a column outside the allowed ones changed
"""

from __future__ import annotations

import json
import sys
import tomllib
from collections import Counter
from collections.abc import Mapping
from pathlib import Path
from typing import Any

GOLDEN_DIR = Path(__file__).parents[1] / "tests" / "data" / "stm32" / "resolution"
PROBES = GOLDEN_DIR / "probes.toml"
FAMILIES = {"STM32U5": "u5", "STM32H5": "h5", "STM32L1": "l1", "STM32L4": "l4"}
COLUMNS = ("label", "source", "factory", "variant", "reads")


def family_of(part: str) -> str:
    """The family key of a part name, such as `u5` for `STM32U575QGIx`."""
    if part[:7] not in FAMILIES:
        raise KeyError(f"{part}: no entry for {part[:7]} in resolution_diff.FAMILIES")
    return FAMILIES[part[:7]]


def probes() -> list[dict[str, str]]:
    """The synthetic probes: a shipped base description renamed to an unshipped part."""
    with PROBES.open("rb") as handle:
        return list(tomllib.load(handle)["probe"])


def resolve_part(part: str, rename: str | None = None) -> dict[str, list[Any]]:
    """Map each non-alias instance of @p part to `[label, source, factory, variant, reads]`.

    @p rename resolves the description under another part name.
    """
    from popcorn import _core
    from popcorn.stm32 import mcu

    description = mcu(part)
    if rename is not None:
        description.part = rename
    return {
        r.instance: [r.row_label, r.source, r.factory, r.variant, r.reads]
        for r in _core.resolve_rows(description)
    }


def compute(family: str) -> dict[str, Any]:
    """The golden of one family: the row table header, the shipped parts and the probes."""
    from popcorn.stm32 import shipped_parts

    groups: dict[str, dict[str, Any]] = {}
    probed: dict[str, dict[str, Any]]
    for part in shipped_parts():
        if family_of(part) != family:
            continue
        resolved = resolve_part(part)
        key = json.dumps(resolved, sort_keys=True)
        groups.setdefault(key, {"parts": [], "instances": resolved})["parts"].append(part)
    probed = {
        p["name"]: {"part": p["part"], "instances": resolve_part(p["base"], p["part"])}
        for p in probes()
        if family_of(p["part"]) == family
    }
    served: dict[str, set[str]] = {}
    tables: list[dict[str, list[Any]]] = [g["instances"] for g in groups.values()]
    tables.extend(p["instances"] for p in probed.values())
    for table in tables:
        for entry in table.values():
            if entry[0]:
                served.setdefault(entry[0], set()).add(family)
    return {
        "family": family,
        "rows": {label: sorted(found) for label, found in sorted(served.items())},
        "groups": sorted(groups.values(), key=lambda g: g["parts"][0]),
        "probes": probed,
    }


def _instances(instances: Mapping[str, list[Any]], indent: str) -> str:
    lines = [f"{indent}  {json.dumps(k)}: {json.dumps(v)}" for k, v in instances.items()]
    return "{\n" + ",\n".join(lines) + f"\n{indent}}}"


def dumps(golden: Mapping[str, Any]) -> str:
    """The golden as text, one line per instance, stable key order."""
    groups = ",\n".join(
        '    {"parts": '
        + json.dumps(g["parts"])
        + ', "instances": '
        + _instances(g["instances"], "    ")
        + "}"
        for g in golden["groups"]
    )
    probed = ",\n".join(
        f'    {json.dumps(name)}: {{"part": {json.dumps(p["part"])}, "instances": '
        + _instances(p["instances"], "    ")
        + "}"
        for name, p in golden["probes"].items()
    )
    rows = ",\n".join(f"    {json.dumps(k)}: {json.dumps(v)}" for k, v in golden["rows"].items())
    return (
        "{\n"
        f'  "family": {json.dumps(golden["family"])},\n'
        f'  "rows": {{\n{rows}\n  }},\n'
        f'  "groups": [\n{groups}\n  ],\n'
        f'  "probes": {{\n{probed}\n  }}\n'
        "}\n"
    )


def update(directory: Path = GOLDEN_DIR) -> None:
    """Rewrite the golden of every family in @p directory."""
    directory.mkdir(parents=True, exist_ok=True)
    for family in FAMILIES.values():
        (directory / f"{family}.json").write_text(dumps(compute(family)))


def flatten(golden: Mapping[str, Any]) -> dict[str, list[Any]]:
    """Map `<part or probe>/<instance>` to `[index, label, source]`."""
    flat: dict[str, list[Any]] = {}
    for group in golden["groups"]:
        for part in group["parts"]:
            for instance, entry in group["instances"].items():
                flat[f"{part}/{instance}"] = entry
    for name, probe in golden["probes"].items():
        for instance, entry in probe["instances"].items():
            flat[f"probe:{name}/{instance}"] = entry
    return flat


def load(directory: Path) -> dict[str, list[Any]]:
    """Every family golden of @p directory, flattened."""
    flat: dict[str, list[Any]] = {}
    for path in sorted(directory.glob("*.json")):
        flat.update(flatten(json.loads(path.read_text())))
    return flat


def changes(
    old: Mapping[str, list[Any]], new: Mapping[str, list[Any]]
) -> dict[str, Counter[tuple[str, str]]]:
    """Per column, how often each old to new value pair occurs. Keys added or removed count
    under `keys`."""
    found: dict[str, Counter[tuple[str, str]]] = {c: Counter() for c in (*COLUMNS, "keys")}
    for key in sorted(set(old) ^ set(new)):
        found["keys"][("present" if key in old else "absent", key.split("/")[0])] += 1
    for key in sorted(set(old) & set(new)):
        for number, column in enumerate(COLUMNS):
            before = old[key][number] if number < len(old[key]) else None
            after = new[key][number] if number < len(new[key]) else None
            if before != after:
                found[column][(str(before), str(after))] += 1
    return found


def summary(found: Mapping[str, Counter[tuple[str, str]]]) -> str:
    """The changes as text: per column, one line per distinct old to new pair with its count,
    then the total."""
    lines: list[str] = []
    for column, pairs in found.items():
        lines.extend(f"{column}: {old!r} -> {new!r} x{n}" for (old, new), n in pairs.items())
        lines.append(f"{column}: {sum(pairs.values())} changed")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if argv[:1] == ["update"]:
        update(Path(argv[1]) if len(argv) > 1 else GOLDEN_DIR)
        return 0
    if argv[:1] == ["diff"] and len(argv) >= 3:
        allowed: set[str] = set(argv[4:]) if argv[3:4] == ["--allow"] else set()
        if len(argv) > 3 and argv[3:4] != ["--allow"]:
            print(__doc__, file=sys.stderr)
            return 2
        found = changes(load(Path(argv[1])), load(Path(argv[2])))
        print(summary(found))
        return 1 if any(n for column, n in found.items() if column not in allowed and n) else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
