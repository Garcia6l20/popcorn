"""Patch file format, loader and application: peripheral drops, fields, cross rules, aliases,
IP renames, IRQ add/drop, core overrides. popcorn's own TOML format, so these dataclasses are the
format definition, not a mirror of any C++ or schema type."""

import copy
import fnmatch
import tomllib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from popcorn.stm32.description._binder import from_mapping
from popcorn.stm32.description._cubemx import DbIp
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    CoreDesc,
    CrossEffect,
    CrossRule,
    FieldDesc,
    IrqDesc,
    PeripheralDesc,
    ReadEffect,
    RegisterDesc,
    WriteEffect,
)


@dataclass(slots=True, kw_only=True)
class PatchMatch:
    parts: list[str]
    svd: list[str] | None = None


@dataclass(slots=True, kw_only=True)
class PatchCore:
    source: str
    reset_clock_hz: int | None = None


@dataclass(slots=True, kw_only=True)
class PatchAliases:
    prefix: str
    offset: int
    source: str | None = None


@dataclass(slots=True, kw_only=True)
class PatchIp:
    renames: dict[str, list[str]]


@dataclass(slots=True, kw_only=True)
class PatchSvd:
    name: str


@dataclass(slots=True, kw_only=True)
class PatchPeripherals:
    drop: list[str]
    source: str


@dataclass(slots=True, kw_only=True)
class IrqAdd:
    number: int
    name: str
    peripheral: str


@dataclass(slots=True, kw_only=True)
class PatchIrqs:
    drop: list[str] | None = None
    add: list[IrqAdd] | None = None


@dataclass(slots=True, kw_only=True)
class EachRange:
    n: list[int]


_INSTANCE_SELECTOR = {"selector_group": "instance_selector"}


@dataclass(slots=True, kw_only=True)
class FieldsRule:
    registers: list[str]
    fields: list[str]
    source: str
    ip: str | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    instances: list[str] | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    on_write: WriteEffect | None = None
    on_read: ReadEffect | None = None
    each: EachRange | None = None


@dataclass(slots=True, kw_only=True)
class CrossRuleSpec:
    reg: str
    target_reg: str
    effect: CrossEffect
    pairs: dict[str, str]
    source: str
    ip: str | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    instances: list[str] | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    each: EachRange | None = None


@dataclass(slots=True, kw_only=True)
class Patch:
    format: int = field(default=1, metadata={"const": 1})
    match: PatchMatch
    source: str | None = None
    core: PatchCore | None = None
    aliases: PatchAliases | None = None
    ip: PatchIp | None = None
    peripherals: PatchPeripherals | None = None
    irqs: PatchIrqs | None = None
    svd: PatchSvd | None = None
    fields: list[FieldsRule] | None = None
    cross: list[CrossRuleSpec] | None = None


@dataclass(slots=True, kw_only=True)
class PatchTarget:
    peripherals: list[PeripheralDesc]
    irqs: list[IrqDesc]
    core: CoreDesc
    db_ips: Mapping[str, DbIp] = field(default_factory=dict[str, DbIp])


@dataclass(slots=True, kw_only=True)
class PatchReport:
    peripherals_dropped: int = 0
    ip_renamed: int = 0
    aliased: int = 0
    fields_touched: int = 0
    cross_rules_added: int = 0
    irqs_dropped: int = 0
    irqs_added: int = 0
    core_fields_set: int = 0


def _matches(match: PatchMatch, part: str, svd_name: str) -> bool:
    if not any(fnmatch.fnmatch(part, pattern) for pattern in match.parts):
        return False
    svd_patterns = match.svd or []
    if not svd_patterns:
        return True
    return any(fnmatch.fnmatch(svd_name, pattern) for pattern in svd_patterns)


@dataclass(slots=True, kw_only=True)
class LoadedPatch:
    path: Path
    patch: Patch


def load_patches(files: Sequence[Path], part: str, svd_name: str) -> list[LoadedPatch]:
    """Patches whose [match] selects @p part and @p svd_name, in the order @p files lists
    them (family files before part files is the caller's ordering of @p files)."""
    result: list[LoadedPatch] = []
    for path in files:
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as exc:
            raise DescriptionError(str(path), exc.lineno or 0, "<root>", "valid TOML") from exc
        patch = from_mapping(Patch, data, file=str(path))
        if _matches(patch.match, part, svd_name):
            result.append(LoadedPatch(path=path, patch=patch))
    return result


def _selector_targets(
    ip: str | None,
    instances: list[str] | None,
    peripherals: dict[str, PeripheralDesc],
    *,
    file: str,
    key: str,
) -> list[PeripheralDesc]:
    if instances is not None:
        matched: list[PeripheralDesc] = []
        for name in instances:
            peripheral = peripherals.get(name)
            if peripheral is None:
                raise DescriptionError(file, 0, f"{key}.instances", f"an instance named {name}")
            matched.append(peripheral)
    else:
        matched = [p for p in peripherals.values() if p.ip == ip]
    if not matched:
        raise DescriptionError(file, 0, key, "at least one matching instance")
    return matched


def _expand_each(each: EachRange | None, *, file: str, key: str) -> list[str]:
    if each is None:
        return [""]
    if len(each.n) != 2 or each.n[0] > each.n[1]:
        raise DescriptionError(file, 0, f"{key}.each.n", "[low, high] with low <= high")
    return [str(n) for n in range(each.n[0], each.n[1] + 1)]


def _find_register(peripheral: PeripheralDesc, name: str, *, file: str, key: str) -> RegisterDesc:
    for register in peripheral.registers:
        if register.name == name:
            return register
    raise DescriptionError(file, 0, key, f"a register named {name} in {peripheral.name}")


def _find_field(register: RegisterDesc, name: str, *, file: str, key: str) -> FieldDesc:
    for f in register.fields:
        if f.name == name:
            return f
    raise DescriptionError(file, 0, key, f"a field named {name} in {register.name}")


def _apply_ip_renames(
    rule: PatchIp,
    peripherals: dict[str, PeripheralDesc],
    db_ips: Mapping[str, DbIp],
    report: PatchReport,
    *,
    file: str,
) -> None:
    for db_instance, targets in rule.renames.items():
        db_ip = db_ips.get(db_instance)
        if db_ip is None:
            raise DescriptionError(file, 0, f"ip.renames.{db_instance}", "a db IP instance")
        for target_name in targets:
            peripheral = peripherals.get(target_name)
            if peripheral is None:
                raise DescriptionError(
                    file, 0, f"ip.renames.{db_instance}", f"an SVD peripheral named {target_name}"
                )
            peripheral.ip = db_ip.name
            peripheral.ip_version = db_ip.version
            report.ip_renamed += 1


def _register_layout_key(peripheral: PeripheralDesc) -> list[object]:
    return [
        (r.name, r.offset, tuple((f.name, f.lsb, f.width) for f in r.fields))
        for r in peripheral.registers
    ]


def _apply_aliases(
    rule: PatchAliases,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
) -> None:
    for name, peripheral in list(peripherals.items()):
        if not name.startswith(rule.prefix):
            continue
        base_name = name[len(rule.prefix) :]
        base_peripheral = peripherals.get(base_name)
        if base_peripheral is None:
            raise DescriptionError(file, 0, f"aliases.{name}", f"a peripheral named {base_name}")
        if peripheral.base != base_peripheral.base + rule.offset:
            raise DescriptionError(
                file,
                0,
                f"aliases.{name}.base",
                f"{base_peripheral.base + rule.offset:#x}",
            )
        if _register_layout_key(peripheral) != _register_layout_key(base_peripheral):
            raise DescriptionError(
                file, 0, f"aliases.{name}.registers", f"the same layout as {base_name}"
            )
        peripheral.alias_of = base_name
        peripheral.ip = base_peripheral.ip
        peripheral.ip_version = base_peripheral.ip_version
        report.aliased += 1


def _apply_fields(
    rule: FieldsRule,
    peripherals: dict[str, PeripheralDesc],
    touched: set[tuple[str, str, str, str]],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    targets = _selector_targets(rule.ip, rule.instances, peripherals, file=file, key=key)
    suffixes = _expand_each(rule.each, file=file, key=key)
    for peripheral in targets:
        for register_template in rule.registers:
            for suffix in suffixes:
                register_name = register_template.replace("{n}", suffix)
                register = _find_register(
                    peripheral, register_name, file=file, key=f"{key}.registers"
                )
                for field_name in rule.fields:
                    field_desc = _find_field(register, field_name, file=file, key=f"{key}.fields")
                    if rule.on_write is not None:
                        marker = (peripheral.name, register.name, field_desc.name, "on_write")
                        if marker in touched:
                            raise DescriptionError(
                                file, 0, f"{key}.on_write", "a field not already patched"
                            )
                        touched.add(marker)
                        field_desc.on_write = rule.on_write
                        report.fields_touched += 1
                    if rule.on_read is not None:
                        marker = (peripheral.name, register.name, field_desc.name, "on_read")
                        if marker in touched:
                            raise DescriptionError(
                                file, 0, f"{key}.on_read", "a field not already patched"
                            )
                        touched.add(marker)
                        field_desc.on_read = rule.on_read
                        report.fields_touched += 1


def _apply_cross(
    rule: CrossRuleSpec,
    peripherals: dict[str, PeripheralDesc],
    touched: set[tuple[str, str, str, str, str]],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    targets = _selector_targets(rule.ip, rule.instances, peripherals, file=file, key=key)
    suffixes = _expand_each(rule.each, file=file, key=key)
    for peripheral in targets:
        for suffix in suffixes:
            reg_name = rule.reg.replace("{n}", suffix)
            target_reg_name = rule.target_reg.replace("{n}", suffix)
            reg = _find_register(peripheral, reg_name, file=file, key=f"{key}.reg")
            target_reg = _find_register(
                peripheral, target_reg_name, file=file, key=f"{key}.target_reg"
            )
            for src_field, dst_field in rule.pairs.items():
                _find_field(reg, src_field, file=file, key=f"{key}.pairs")
                _find_field(target_reg, dst_field, file=file, key=f"{key}.pairs")
                marker = (peripheral.name, reg.name, src_field, target_reg.name, dst_field)
                if marker in touched:
                    raise DescriptionError(
                        file, 0, f"{key}.pairs", "a cross rule not already patched"
                    )
                touched.add(marker)
                peripheral.cross_rules.append(
                    CrossRule(
                        reg=reg.name,
                        field=src_field,
                        target_reg=target_reg.name,
                        target_field=dst_field,
                        effect=rule.effect,
                    )
                )
                report.cross_rules_added += 1


def _apply_irqs(
    rule: PatchIrqs,
    peripherals: dict[str, PeripheralDesc],
    irqs: list[IrqDesc],
    report: PatchReport,
    *,
    file: str,
) -> None:
    for name in rule.drop or []:
        matching = [irq for irq in irqs if irq.name == name]
        if not matching:
            raise DescriptionError(file, 0, f"irqs.drop.{name}", "a known IRQ name")
        number = matching[0].number
        irqs[:] = [irq for irq in irqs if irq.name != name]
        for peripheral in peripherals.values():
            if number in peripheral.irqs:
                peripheral.irqs = [n for n in peripheral.irqs if n != number]
        report.irqs_dropped += 1

    for add in rule.add or []:
        if any(irq.number == add.number for irq in irqs):
            raise DescriptionError(file, 0, f"irqs.add.{add.name}", "an unused IRQ number")
        peripheral = peripherals.get(add.peripheral)
        if peripheral is None:
            raise DescriptionError(
                file, 0, f"irqs.add.{add.name}", f"a peripheral named {add.peripheral}"
            )
        irqs.append(IrqDesc(number=add.number, name=add.name))
        if add.number not in peripheral.irqs:
            peripheral.irqs = [*peripheral.irqs, add.number]
        report.irqs_added += 1


def _apply_drops(
    rules: Sequence[tuple[PatchPeripherals, str]],
    peripherals: list[PeripheralDesc],
    report: PatchReport,
) -> None:
    known = {p.name for p in peripherals}
    dropped: set[str] = set()
    for rule, file in rules:
        for name in rule.drop:
            if name in dropped:
                raise DescriptionError(file, 0, "peripherals.drop", "each name once")
            if name not in known:
                raise DescriptionError(file, 0, f"peripherals.drop.{name}", "a known peripheral")
            dropped.add(name)
    peripherals[:] = [p for p in peripherals if p.name not in dropped]
    report.peripherals_dropped += len(dropped)


def _apply_core(
    rule: PatchCore, core: CoreDesc, touched: set[str], report: PatchReport, *, file: str
) -> None:
    if rule.reset_clock_hz is not None:
        if "reset_clock_hz" in touched:
            raise DescriptionError(file, 0, "core.reset_clock_hz", "not already patched")
        touched.add("reset_clock_hz")
        core.reset_clock_hz = rule.reset_clock_hz
        report.core_fields_set += 1


def apply_patches(patches: Sequence[LoadedPatch], target: PatchTarget) -> PatchReport:
    """Applies @p patches to @p target in the fixed order: peripheral drops, IP renames, aliases,
    fields, cross rules, IRQ drops and adds, core. Drops come first, so a later rule naming a
    dropped peripheral fails as naming an unknown one. Validates every rule before mutating
    @p target: an error leaves it exactly as it was."""
    peripherals = copy.deepcopy(target.peripherals)
    irqs = copy.deepcopy(target.irqs)
    core = copy.deepcopy(target.core)
    report = PatchReport()

    _apply_drops(
        [
            (loaded.patch.peripherals, str(loaded.path))
            for loaded in patches
            if loaded.patch.peripherals is not None
        ],
        peripherals,
        report,
    )

    by_name = {p.name: p for p in peripherals}

    for loaded in patches:
        if loaded.patch.ip is not None:
            _apply_ip_renames(
                loaded.patch.ip, by_name, target.db_ips, report, file=str(loaded.path)
            )

    for loaded in patches:
        if loaded.patch.aliases is not None:
            _apply_aliases(loaded.patch.aliases, by_name, report, file=str(loaded.path))

    fields_touched: set[tuple[str, str, str, str]] = set()
    for loaded in patches:
        for j, rule in enumerate(loaded.patch.fields or []):
            _apply_fields(
                rule,
                by_name,
                fields_touched,
                report,
                file=str(loaded.path),
                key=f"fields[{j}]",
            )

    cross_touched: set[tuple[str, str, str, str, str]] = set()
    for loaded in patches:
        for j, rule in enumerate(loaded.patch.cross or []):
            _apply_cross(
                rule,
                by_name,
                cross_touched,
                report,
                file=str(loaded.path),
                key=f"cross[{j}]",
            )

    for loaded in patches:
        if loaded.patch.irqs is not None:
            _apply_irqs(loaded.patch.irqs, by_name, irqs, report, file=str(loaded.path))

    core_touched: set[str] = set()
    for loaded in patches:
        if loaded.patch.core is not None:
            _apply_core(loaded.patch.core, core, core_touched, report, file=str(loaded.path))

    target.peripherals = peripherals
    target.irqs = irqs
    target.core = core
    return report
