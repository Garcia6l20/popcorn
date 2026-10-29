"""Canonical text dumps of an PartDescription and the golden digest file they round-trip through.

Table format, one line per item, in description order (already deterministic, spec 04):
  P name base size ip bonded bonded_channels
  R periph reg offset width access reset mask nfields alternate_of aliases
  F periph reg field lsb width access on_write on_read
  I number name
  A periph pin af signal
  G name start size kind read write execute secure physical content
  C cpu revision fpu fpu_double mpu mpu_regions mpu_regions_secure sau_regions trustzone dsp
    nvic_prio_bits num_irqs vendor_systick_config reset_clock_hz
Addresses, offsets, resets and masks are lower-case hex; every other number is decimal; a missing
optional value is "-". Register aliases are joined by commas. Region content is offset:hexbytes
pieces joined by commas.
"""

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import TypedDict, cast

from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._patches import PatchReport

_TABLES = ("peripherals", "registers", "fields", "regions", "irqs", "core", "alternates")


class GoldenCountsDict(TypedDict):
    peripherals: int
    registers: int
    fields: int
    regions: int
    irqs: int
    aliases: int
    cross_rules: int
    patched_fields: int
    alternates: int


class GoldenTablesDict(TypedDict):
    peripherals: str
    registers: str
    fields: str
    regions: str
    irqs: str
    core: str
    alternates: str


class GoldenFile(TypedDict):
    part: str
    svd_sha256: str
    counts: GoldenCountsDict
    tables: GoldenTablesDict


def _dump_peripherals(description: PartDescription) -> str:
    lines = [
        f"P {p.name} {p.base:#010x} {p.size:#010x} {p.ip} "
        f"{f'{p.bonded:#06x}' if p.bonded is not None else '-'} "
        f"{f'{p.bonded_channels:#010x}' if p.bonded_channels is not None else '-'}"
        for p in description.peripherals
    ]
    return "\n".join(lines) + "\n"


def _dump_registers(description: PartDescription) -> str:
    lines: list[str] = []
    for p in description.peripherals:
        for r in p.registers:
            alt = r.alternate_of if r.alternate_of is not None else "-"
            aliases = ",".join(r.aliases) if r.aliases else "-"
            lines.append(
                f"R {p.name} {r.name} {r.offset:#06x} {r.width} {r.acc.value} "
                f"{r.reset:#010x} {r.reset_mask:#010x} {len(r.fields)} {alt} {aliases}"
            )
    return "\n".join(lines) + "\n"


def _dump_fields(description: PartDescription) -> str:
    lines: list[str] = []
    for p in description.peripherals:
        for r in p.registers:
            for f in r.fields:
                lines.append(
                    f"F {p.name} {r.name} {f.name} {f.lsb} {f.width} {f.acc.value} "
                    f"{f.on_write.value} {f.on_read.value}"
                )
    return "\n".join(lines) + "\n"


def _dump_irqs(description: PartDescription) -> str:
    lines = [f"I {irq.number} {irq.name}" for irq in description.irqs]
    return "\n".join(lines) + "\n"


def _dump_alternates(description: PartDescription) -> str:
    lines = [
        f"A {p.name} {a.pin} {a.af} {a.signal}"
        for p in description.peripherals
        for a in p.alternates or []
    ]
    return "\n".join(lines) + "\n"


def _dump_regions(description: PartDescription) -> str:
    lines: list[str] = []
    for r in description.regions:
        physical = f"{r.physical:#010x}" if r.physical is not None else "-"
        content = (
            ",".join(f"{piece.offset:#x}:{bytes(piece.data).hex()}" for piece in r.content)
            if r.content
            else "-"
        )
        lines.append(
            f"G {r.name} {r.start:#010x} {r.size:#010x} {r.kind.value} "
            f"{int(r.acc.read)} {int(r.acc.write)} {int(r.acc.execute)} {int(r.secure)} {physical} "
            f"{content}"
        )
    return "\n".join(lines) + "\n"


def _dump_core(description: PartDescription) -> str:
    core = description.core
    return (
        f"C {core.cpu} {core.revision} {int(core.fpu)} {int(core.fpu_double)} {int(core.mpu)} "
        f"{core.mpu_regions} {core.mpu_regions_secure} {core.sau_regions} {int(core.trustzone)} "
        f"{int(core.dsp)} {core.nvic_prio_bits} {core.num_irqs} "
        f"{int(core.vendor_systick_config)} {core.reset_clock_hz}\n"
    )


_DUMPERS = {
    "peripherals": _dump_peripherals,
    "registers": _dump_registers,
    "fields": _dump_fields,
    "regions": _dump_regions,
    "irqs": _dump_irqs,
    "core": _dump_core,
    "alternates": _dump_alternates,
}


def canonical_dumps(description: PartDescription) -> dict[str, str]:
    """The seven canonical text tables of @p description, keyed peripherals/registers/fields/
    regions/irqs/core/alternates."""
    return {name: _DUMPERS[name](description) for name in _TABLES}


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


@dataclass(slots=True, frozen=True, kw_only=True)
class GoldenCounts:
    peripherals: int
    registers: int
    fields: int
    regions: int
    irqs: int
    aliases: int
    cross_rules: int
    patched_fields: int
    alternates: int


def compute_counts(description: PartDescription, report: PatchReport) -> GoldenCounts:
    """Item counts of @p description, plus @p report's cross_rules_added/fields_touched."""
    registers = sum(len(p.registers) for p in description.peripherals)
    fields = sum(len(r.fields) for p in description.peripherals for r in p.registers)
    aliases = sum(1 for p in description.peripherals if p.alias_of is not None)
    alternates = sum(len(p.alternates or []) for p in description.peripherals)
    return GoldenCounts(
        peripherals=len(description.peripherals),
        registers=registers,
        fields=fields,
        regions=len(description.regions),
        irqs=len(description.irqs),
        aliases=aliases,
        cross_rules=report.cross_rules_added,
        patched_fields=report.fields_touched,
        alternates=alternates,
    )


def compute_golden(
    description: PartDescription, report: PatchReport, *, svd_sha256: str
) -> GoldenFile:
    """The golden JSON shape for @p description: part, svd_sha256, counts and per-table
    digests."""
    counts = compute_counts(description, report)
    dumps = canonical_dumps(description)
    tables = cast(GoldenTablesDict, {name: _sha256_text(text) for name, text in dumps.items()})
    return GoldenFile(
        part=description.part,
        svd_sha256=svd_sha256,
        counts=GoldenCountsDict(
            peripherals=counts.peripherals,
            registers=counts.registers,
            fields=counts.fields,
            regions=counts.regions,
            irqs=counts.irqs,
            aliases=counts.aliases,
            cross_rules=counts.cross_rules,
            patched_fields=counts.patched_fields,
            alternates=counts.alternates,
        ),
        tables=tables,
    )


def load_golden(path: Path) -> GoldenFile:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return cast(GoldenFile, data)


def write_golden(path: Path, golden: GoldenFile) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(golden, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def dump_tables(directory: Path, description: PartDescription) -> None:
    """Writes each canonical table of @p description as <directory>/<table>.txt."""
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in canonical_dumps(description).items():
        (directory / f"{name}.txt").write_text(text, encoding="utf-8")
