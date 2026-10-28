"""Generate the support pages of the docs site from the shipped descriptions and the model registry.

Run in its own interpreter, `python -m popcorn_build.support_pages <out_dir>`, so only the built-in
models are registered.
"""

import os
import sys
from collections.abc import Iterable
from pathlib import Path

import popcorn
from popcorn import PartDescription
from popcorn.stm32 import PeripheralSupport, mcu, shipped_parts, support

LOCAL_SOURCES = ("POPCORN_STM32_DESCRIPTION_DIRS", "POPCORN_STM32_BUILD")
"""Variables that make `mcu()` read a description other than the shipped one."""

BUILTIN = "built-in"
TRUSTZONE_ONLY = "built-in, TrustZone only"
NONE = "none"


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
    ]
    return "\n".join(lines)


def parts_page(parts: dict[str, PartDescription]) -> str:
    """The list of shipped parts, with core and memory, each linking to its peripheral page."""
    rows: list[list[str]] = []
    for name, part in parts.items():
        models = _models(part)
        rows.append(
            [
                f"[{name}]({name}.md)",
                part.core.cpu,
                "yes" if part.core.trustzone else "no",
                _kib(_memory(part, "rom", "Flash")),
                _kib(_memory(part, "ram", "SRAM")),
                f"{sum(1 for m in models.values() if m != NONE)} of {len(models)}",
            ]
        )
    lines = [
        "# Parts",
        "",
        "These parts ship with a description, so `popcorn.stm32.mcu()` loads them without ST's "
        "files. Flash and SRAM count the Non-secure aliases only, backup SRAM included. "
        "Modelled counts the instances with a built-in model, TrustZone-only models included.",
        "",
        *_table(["Part", "Core", "TrustZone", "Flash", "SRAM", "Modelled"], rows),
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
    pages = {"parts.md": parts_page(parts), "cores.md": cores_page(parts)}
    pages.update({f"{name}.md": part_page(part) for name, part in parts.items()})
    written: list[Path] = []
    for file, text in pages.items():
        path = out / file
        path.write_text(text)
        written.append(path)
    return written


if __name__ == "__main__":
    write(Path(sys.argv[1]))
