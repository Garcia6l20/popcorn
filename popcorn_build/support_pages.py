"""Generate the support pages of the docs site from the shipped descriptions and the model registry.

Run in its own interpreter, `python -m popcorn_build.support_pages <out_dir>`, so only the built-in
models are registered.
"""

import os
import sys
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path

import popcorn
from popcorn import PartDescription
from popcorn.stm32 import PeripheralSupport, mcu, shipped_parts, support

from popcorn_build.example_parts import (
    EXAMPLE_PARTS,
    GEOMETRY_PARTS,
    TESTED_PARTS,
    ExampleParts,
    PartClass,
    part_class,
)

LOCAL_SOURCES = ("POPCORN_STM32_DESCRIPTION_DIRS", "POPCORN_STM32_BUILD")
"""Variables that make `mcu()` read a description other than the shipped one."""

BUILTIN = "built-in"
TRUSTZONE_ONLY = "built-in, TrustZone only"
NONE = "none"

TESTED = "tested"
EXAMPLES = "examples"
CLASS = "class"
ATTACHES = "attaches"
GEOMETRY_NOTE = "FLASH geometry cases"


@dataclass(frozen=True, slots=True)
class PartState:
    """The evidence behind one shipped part.

    Attributes:
        state: `tested`, `examples`, `class` or `attaches`.
        detail: For `examples` the examples that run on the part, for `class` the part of its
            class that runs examples, empty otherwise.
        geometry: Whether the FLASH geometry cases run on the part.
    """

    state: str
    detail: tuple[str, ...] = ()
    geometry: bool = False


def _kib(size: int) -> str:
    return f"{size // 1024} KiB"


def _memory(part: PartDescription, kind: str, name: str) -> int:
    return sum(
        r.size
        for r in part.regions
        if r.kind.name == kind and name in r.name and not r.name.endswith("_S")
    )


def _models(part: PartDescription) -> dict[str, str]:
    """Map each peripheral to its model column."""
    plain = {s.name: s for s in support(part)}
    secured: dict[str, PeripheralSupport] = plain
    if part.core.trustzone:
        secured = {s.name: s for s in support(part, trustzone=True)}
    found: dict[str, str] = {}
    for name, entry in plain.items():
        if entry.model == "builtin":
            found[name] = BUILTIN
        elif secured[name].model == "builtin":
            found[name] = TRUSTZONE_ONLY
        else:
            found[name] = NONE
    return found


def _table(header: Iterable[str], rows: Iterable[Iterable[str]]) -> list[str]:
    head = list(header)
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def part_states(
    shipped: Iterable[str],
    *,
    tested: Iterable[str] = TESTED_PARTS,
    examples: Mapping[str, ExampleParts] = EXAMPLE_PARTS,
    geometry: Iterable[str] = GEOMETRY_PARTS,
) -> dict[str, PartState]:
    """The state of each of @p shipped, in order.

    `tested` holds a part of @p tested, `examples` a part some example in @p examples lists in
    `extra`, `class` a part whose class holds one of those, and `attaches` any other. A part
    of @p tested, of an `extra` or of @p geometry that is not in @p shipped is a ValueError
    naming it.
    """
    names = list(shipped)
    tested_parts = frozenset(tested)
    ran: dict[str, list[str]] = {}
    for example, row in examples.items():
        for part in row.extra:
            ran.setdefault(part, []).append(example)
    geometry_parts = frozenset(geometry)
    missing = sorted({*tested_parts, *ran, *geometry_parts} - set(names))
    if missing:
        raise ValueError(f"{', '.join(missing)}: runs examples or tests, ships no description")
    by_class: dict[PartClass, list[str]] = {}
    for part in sorted({*tested_parts, *ran}):
        by_class.setdefault(part_class(part), []).append(part)
    states: dict[str, PartState] = {}
    for name in names:
        note = name in geometry_parts
        if name in tested_parts:
            states[name] = PartState(TESTED, geometry=note)
        elif name in ran:
            states[name] = PartState(EXAMPLES, tuple(ran[name]), note)
        elif running := by_class.get(part_class(name.partition("-")[0])):
            states[name] = PartState(CLASS, (running[0],), note)
        else:
            states[name] = PartState(ATTACHES, geometry=note)
    return states


def _state_cell(state: PartState) -> str:
    text = state.state
    if state.state == EXAMPLES:
        text += f": {', '.join(state.detail)}"
    elif state.state == CLASS:
        text += f" of {state.detail[0]}"
    if state.geometry:
        text += f", {GEOMETRY_NOTE}"
    return text


def _pins(port: str, mask: int) -> str:
    if not mask:
        return "-"
    runs: list[str] = []
    number = 0
    while number < 16:
        if not mask >> number & 1:
            number += 1
            continue
        last = number
        while last < 15 and mask >> (last + 1) & 1:
            last += 1
        first, end = f"P{port}{number}", f"P{port}{last}"
        runs.append(first if number == last else f"{first}-{end}")
        number = last + 1
    return ", ".join(runs)


def _bonded(part: PartDescription) -> list[list[str]]:
    rows: list[list[str]] = []
    for p in part.peripherals:
        if p.name.startswith("GPIO") and p.alias_of is None:
            port = p.name[4:]
            if p.bonded is None:
                raise ValueError(f"{part.part}: {p.name} has no bonded mask")
            mask = p.bonded
            rows.append([p.name, _pins(port, mask), str(mask.bit_count())])
    return rows


def part_page(part: PartDescription) -> str:
    """The peripheral page of one part: one row per IP and version, its instances and model."""
    models = _models(part)
    groups: dict[tuple[str, str, str], list[str]] = {}
    for p in part.peripherals:
        if p.alias_of is None:
            groups.setdefault((p.ip, p.ip_version, models[p.name]), []).append(p.name)
    served = sum(1 for m in models.values() if m == BUILTIN)
    secured = sum(1 for m in models.values() if m == TRUSTZONE_ONLY)
    order = {BUILTIN: 0, TRUSTZONE_ONLY: 1, NONE: 2}
    keys = sorted(groups, key=lambda key: (order[key[2]], key[0].lower(), key[1]))
    rows = [
        [f"`{ip}`", f"`{version}`" if version else "", ", ".join(groups[ip, version, model]), model]
        for ip, version, model in keys
    ]
    lines = [
        f"# {part.part}",
        "",
        f"Core {part.core.cpu}. {len(models)} peripheral instances: {served} have a built-in "
        f"model, {secured} more have one only with TrustZone enabled.",
        "",
        "Each row is one IP and version, as the description names them. Secure aliases are left "
        "out. See [Limits](../limits.md) for what each model leaves out.",
        "",
        *_table(["IP", "Version", "Instances", "Model"], rows),
        "",
        "## Bonded pins",
        "",
        "The pins of each GPIO port that the package of this part bonds. Board access through a "
        "machine reaches these pins. The port's registers cover every pin of the die.",
        "",
        *_table(["Port", "Pins", "Count"], _bonded(part)),
        "",
    ]
    return "\n".join(lines)


def parts_page(parts: dict[str, PartDescription], states: Mapping[str, PartState]) -> str:
    """The list of shipped parts with line, die, package, memory and state, each linking to its
    peripheral page."""
    rows: list[list[str]] = []
    for name, part in parts.items():
        kind = part_class(name.partition("-")[0])
        rows.append(
            [
                f"[{name}]({name}.md)",
                name[5:9],
                kind.die,
                kind.package,
                _kib(_memory(part, "rom", "Flash")),
                _kib(_memory(part, "ram", "SRAM")),
                "yes" if kind.smps else "no",
                _state_cell(states[name]),
            ]
        )
    lines = [
        "# Parts",
        "",
        "These parts ship with a description, so `popcorn.stm32.mcu()` loads them from the "
        "shipped description alone. Flash and SRAM count the Non-secure aliases only, backup "
        "SRAM included. A part's class is its die, flash size, package and SMPS supply. The dies "
        "are U575/U585 and H56x/H573.",
        "",
        "Every part attaches at TrustZone 0 and 1, and its flash end, SRAM ends and bonded pins "
        "are checked on the host, whatever its state. The state names the firmware evidence:",
        "",
        f"- **{TESTED}**: the part runs the whole firmware suites of the examples.",
        f"- **{EXAMPLES}**: the part runs the listed examples itself.",
        f"- **{CLASS}**: the part shares its class with the named part, which runs examples.",
        f"- **{ATTACHES}**: the part ships and attaches, and its host checks are its evidence.",
        f"- **{GEOMETRY_NOTE}**: the FLASH model's geometry cases run on the part.",
        "",
        f"A part in the {TESTED}, {EXAMPLES} or {CLASS} state is supported.",
        "",
        *_table(["Part", "Line", "Die", "Package", "Flash", "SRAM", "SMPS", "State"], rows),
        "",
    ]
    return "\n".join(lines)


def cores_page(parts: dict[str, PartDescription]) -> str:
    """The arch modules and the core of each shipped part."""
    modules = ", ".join(f"`{m}`" for m in popcorn.arch_modules())
    rows = [
        [
            name,
            part.core.cpu,
            "double" if part.core.fpu_double else "single" if part.core.fpu else "no",
            "yes" if part.core.dsp else "no",
            "yes" if part.core.trustzone else "no",
            (
                f"{part.core.mpu_regions} + {part.core.mpu_regions_secure} Secure"
                if part.core.trustzone
                else str(part.core.mpu_regions)
            ),
            str(part.core.num_irqs),
        ]
        for name, part in parts.items()
    ]
    lines = [
        "# Cores",
        "",
        f"popcorn emulates a core through an arch module. Registered modules: {modules}. A machine "
        "takes the latest registered module that implements its part's core. MPU regions count "
        "the Non-secure MPU, plus the Secure one on a part with TrustZone.",
        "",
        *_table(["Part", "Core", "FPU", "DSP", "TrustZone", "MPU regions", "IRQs"], rows),
        "",
    ]
    return "\n".join(lines)


def write(out: Path) -> list[Path]:
    """Write every generated page into `out` and return their paths. Removes the variables of
    `LOCAL_SOURCES` from the environment first, so the pages show the shipped descriptions."""
    for name in LOCAL_SOURCES:
        os.environ.pop(name, None)
    out.mkdir(parents=True, exist_ok=True)
    parts: dict[str, PartDescription] = {}
    for name in shipped_parts():
        refname, _, core = name.partition("-")
        parts[name] = mcu(refname, core=core or None)
    states = part_states(parts)
    pages = {"parts.md": parts_page(parts, states), "cores.md": cores_page(parts)}
    pages.update({f"{name}.md": part_page(part) for name, part in parts.items()})
    written: list[Path] = []
    for file, text in pages.items():
        path = out / file
        path.write_text(text)
        written.append(path)
    return written


if __name__ == "__main__":
    write(Path(sys.argv[1]))
