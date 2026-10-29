"""Patch file format, loader and application: peripheral drops and renames, fields, cross rules,
aliases, IP renames, IRQ add/drop, GPIO alternate function drops, core overrides, peripheral
sizes, added registers, added memory regions, region controllers, the device signature. popcorn's
own TOML format, so these dataclasses are the format definition, not a mirror of any C++ or
schema type."""

import copy
import fnmatch
import tomllib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
from pathlib import Path

from popcorn.stm32.description._binder import from_mapping
from popcorn.stm32.description._cubemx import DbIp, DbPart
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    AlternateDesc,
    CoreDesc,
    CrossEffect,
    CrossRule,
    FieldDesc,
    IrqDesc,
    PeripheralDesc,
    ReadEffect,
    RegionContent,
    RegionDesc,
    RegionKind,
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
    source: str
    drop: list[str] | None = None
    renames: dict[str, str] | None = None


@dataclass(slots=True, kw_only=True)
class PatchMemory:
    source: str
    add: list[RegionDesc] | None = None
    controller: dict[str, str] | None = None


@dataclass(slots=True, kw_only=True)
class PatchSignature:
    region: str
    flash_size: int = field(metadata={"minimum": 0})
    package: int = field(metadata={"minimum": 0})
    uid: int = field(metadata={"minimum": 0})
    uid_words: list[int]
    packages: dict[str, int]
    source: str
    content: list[RegionContent] | None = None


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
class AlternateDrop:
    peripheral: str
    pin: int = field(metadata={"minimum": 0, "maximum": 15})
    af: int = field(metadata={"minimum": 0, "maximum": 15})
    signal: str
    source: str = field(metadata={"min_length": 1})


@dataclass(slots=True, kw_only=True)
class PatchAlternates:
    drop: list[AlternateDrop]


@dataclass(slots=True, kw_only=True)
class EachRange:
    n: list[int]


_INSTANCE_SELECTOR = {"selector_group": "instance_selector"}


@dataclass(slots=True, kw_only=True)
class FieldAdd:
    name: str
    lsb: int = field(metadata={"minimum": 0, "maximum": 31})
    width: int = field(metadata={"minimum": 1, "maximum": 32})
    acc: Access
    reset: int = field(metadata={"minimum": 0, "maximum": 4294967295})


@dataclass(slots=True, kw_only=True)
class FieldsRule:
    registers: list[str]
    fields: list[str] | None = None
    source: str
    ip: str | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    instances: list[str] | None = field(default=None, metadata=_INSTANCE_SELECTOR)
    on_write: WriteEffect | None = None
    on_read: ReadEffect | None = None
    acc: Access | None = None
    width: int | None = field(default=None, metadata={"minimum": 1, "maximum": 32})
    drop: bool | None = None
    rename: dict[str, str] | None = None
    add: FieldAdd | None = None
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
class AddedField:
    name: str
    lsb: int = field(metadata={"minimum": 0, "maximum": 31})
    width: int = field(metadata={"minimum": 1, "maximum": 32})


@dataclass(slots=True, kw_only=True)
class RegisterAdd:
    peripheral: str
    name: str
    offset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    width: int
    reset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    acc: Access
    fields: list[AddedField]
    source: str


@dataclass(slots=True, kw_only=True)
class PeripheralSizeSet:
    peripheral: str
    size: int = field(metadata={"minimum": 1, "maximum": 4294967295})
    source: str


@dataclass(slots=True, kw_only=True)
class ResetSet:
    peripheral: str
    register: str
    reset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    source: str


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
    alternates: PatchAlternates | None = None
    svd: PatchSvd | None = None
    fields: list[FieldsRule] | None = None
    cross: list[CrossRuleSpec] | None = None
    peripheral_sizes: list[PeripheralSizeSet] | None = None
    registers: list[RegisterAdd] | None = None
    resets: list[ResetSet] | None = None
    memory: PatchMemory | None = None
    signature: PatchSignature | None = None


@dataclass(slots=True, kw_only=True)
class PatchTarget:
    peripherals: list[PeripheralDesc]
    irqs: list[IrqDesc]
    core: CoreDesc
    db_ips: Mapping[str, DbIp] = field(default_factory=dict[str, DbIp])
    regions: list[RegionDesc] = field(default_factory=list[RegionDesc])
    db_part: DbPart | None = None


@dataclass(slots=True, kw_only=True)
class PatchReport:
    peripherals_dropped: int = 0
    peripherals_renamed: int = 0
    ip_renamed: int = 0
    aliased: int = 0
    fields_touched: int = 0
    fields_dropped: int = 0
    fields_renamed: int = 0
    fields_added: int = 0
    cross_rules_added: int = 0
    irqs_dropped: int = 0
    irqs_added: int = 0
    alternates_dropped: int = 0
    sizes_set: int = 0
    registers_added: int = 0
    resets_set: int = 0
    core_fields_set: int = 0
    regions_added: int = 0
    controllers_set: int = 0
    signature_words: int = 0


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


def _apply_peripheral_size(
    rule: PeripheralSizeSet,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    peripheral = peripherals.get(rule.peripheral)
    if peripheral is None or peripheral.alias_of is not None:
        raise DescriptionError(
            file, 0, f"{key}.peripheral", "a peripheral of the description that is not an alias"
        )
    if rule.size < peripheral.size:
        raise DescriptionError(file, 0, f"{key}.size", f"a size of at least {peripheral.size:#x}")
    for target in peripherals.values():
        if target is peripheral or target.alias_of == peripheral.name:
            target.size = rule.size
    report.sizes_set += 1


def _apply_reset(
    rule: ResetSet,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    peripheral = peripherals.get(rule.peripheral)
    if peripheral is None or peripheral.alias_of is not None:
        raise DescriptionError(
            file, 0, f"{key}.peripheral", "a peripheral of the description that is not an alias"
        )
    register = next((r for r in peripheral.registers if r.name == rule.register), None)
    if register is None:
        raise DescriptionError(file, 0, f"{key}.register", f"a register of {peripheral.name}")
    if rule.reset >> register.width:
        raise DescriptionError(file, 0, f"{key}.reset", f"a value of {register.width} bits")
    for target in peripherals.values():
        if target is peripheral or target.alias_of == peripheral.name:
            for candidate in target.registers:
                if candidate.name == rule.register:
                    candidate.reset = rule.reset
                    candidate.reset_mask = (1 << candidate.width) - 1
    report.resets_set += 1


def _apply_register_add(
    rule: RegisterAdd,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    peripheral = peripherals.get(rule.peripheral)
    if peripheral is None or peripheral.alias_of is not None:
        raise DescriptionError(
            file, 0, f"{key}.peripheral", "a peripheral of the description that is not an alias"
        )
    if rule.width not in (8, 16, 32):
        raise DescriptionError(file, 0, f"{key}.width", "8, 16 or 32")
    size = rule.width // 8
    if rule.offset % size != 0 or rule.offset + size > peripheral.size:
        raise DescriptionError(
            file, 0, f"{key}.offset", f"an offset aligned to {size} inside {peripheral.size:#x}"
        )
    if rule.reset >> rule.width:
        raise DescriptionError(file, 0, f"{key}.reset", f"a value of {rule.width} bits")
    for register in peripheral.registers:
        if register.name == rule.name:
            raise DescriptionError(file, 0, f"{key}.name", f"a name not used in {peripheral.name}")
        if (
            rule.offset < register.offset + register.width // 8
            and register.offset < rule.offset + size
        ):
            raise DescriptionError(file, 0, f"{key}.offset", f"a range clear of {register.name}")
    taken = 0
    names: set[str] = set()
    for added in rule.fields:
        if added.name in names:
            raise DescriptionError(file, 0, f"{key}.fields.{added.name}", "each name once")
        names.add(added.name)
        if added.lsb + added.width > rule.width:
            raise DescriptionError(
                file, 0, f"{key}.fields.{added.name}", f"bits inside the {rule.width}-bit register"
            )
        bits = ((1 << added.width) - 1) << added.lsb
        if taken & bits:
            raise DescriptionError(
                file, 0, f"{key}.fields.{added.name}", "bits clear of the other fields"
            )
        taken |= bits
    register = RegisterDesc(
        name=rule.name,
        offset=rule.offset,
        width=rule.width,
        reset=rule.reset,
        reset_mask=(1 << rule.width) - 1,
        acc=rule.acc,
        fields=[
            FieldDesc(
                name=added.name,
                lsb=added.lsb,
                width=added.width,
                acc=rule.acc,
                on_write=WriteEffect.MODIFY,
                on_read=ReadEffect.NONE,
            )
            for added in sorted(rule.fields, key=lambda added: added.lsb, reverse=True)
        ],
    )
    for target in peripherals.values():
        if target is peripheral or target.alias_of == peripheral.name:
            target.registers = sorted(
                [*target.registers, copy.deepcopy(register)], key=lambda r: r.offset
            )
    report.registers_added += 1


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
    if rule.add is not None:
        _add_field(rule, targets, peripherals, suffixes, touched, report, file=file, key=key)
        return
    if rule.rename is not None:
        _rename_fields(rule, targets, peripherals, suffixes, touched, report, file=file, key=key)
        return
    if not rule.fields:
        raise DescriptionError(file, 0, f"{key}.fields", "at least one field name")
    if rule.drop:
        _drop_fields(rule, targets, peripherals, suffixes, touched, report, file=file, key=key)
        return
    if rule.instances is None:
        targets = [target for target in targets if target.alias_of is None]
    for peripheral in targets:
        if peripheral.alias_of is not None:
            raise DescriptionError(
                file,
                0,
                f"{key}.instances",
                f"a peripheral that is not an alias, not {peripheral.name}",
            )
        for register_template in rule.registers:
            for suffix in suffixes:
                register_name = register_template.replace("{n}", suffix)
                register = _find_register(
                    peripheral, register_name, file=file, key=f"{key}.registers"
                )
                for field_name in rule.fields or []:
                    field_desc = _find_field(register, field_name, file=file, key=f"{key}.fields")
                    for prop in ("width", "on_write", "on_read", "acc"):
                        value = getattr(rule, prop)
                        if value is None:
                            continue
                        marker = (peripheral.name, register.name, field_desc.name, prop)
                        if marker in touched:
                            raise DescriptionError(
                                file, 0, f"{key}.{prop}", "a field not already patched"
                            )
                        touched.add(marker)
                        for target in peripherals.values():
                            if target is not peripheral and target.alias_of != peripheral.name:
                                continue
                            same = _find_register(target, register.name, file=file, key=key)
                            same_field = _find_field(same, field_name, file=file, key=key)
                            if prop == "width":
                                _resize_field(same, same_field, value, file=file, key=key)
                            else:
                                setattr(same_field, prop, value)
                            report.fields_touched += 1


def _drop_fields(
    rule: FieldsRule,
    targets: list[PeripheralDesc],
    peripherals: dict[str, PeripheralDesc],
    suffixes: list[str],
    touched: set[tuple[str, str, str, str]],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    if (
        rule.width is not None
        or rule.on_write is not None
        or rule.on_read is not None
        or rule.acc is not None
    ):
        raise DescriptionError(
            file, 0, f"{key}.drop", "a rule with no width, on_write, on_read or acc"
        )
    if not rule.source:
        raise DescriptionError(file, 0, f"{key}.source", "a non-empty source")
    if rule.instances is None:
        targets = [target for target in targets if target.alias_of is None]
    for peripheral in targets:
        if peripheral.alias_of is not None:
            raise DescriptionError(
                file,
                0,
                f"{key}.instances",
                f"a peripheral that is not an alias, not {peripheral.name}",
            )
        for register_template in rule.registers:
            for suffix in suffixes:
                register_name = register_template.replace("{n}", suffix)
                register = _find_register(
                    peripheral, register_name, file=file, key=f"{key}.registers"
                )
                for field_name in rule.fields or []:
                    marker = (peripheral.name, register.name, field_name, "drop")
                    if marker in touched:
                        raise DescriptionError(
                            file, 0, f"{key}.fields", f"{field_name} dropped by no earlier rule"
                        )
                    touched.add(marker)
                    _find_field(register, field_name, file=file, key=f"{key}.fields")
                    for target in peripherals.values():
                        if target is peripheral or target.alias_of == peripheral.name:
                            for same in target.registers:
                                if same.name == register.name:
                                    same.fields = [f for f in same.fields if f.name != field_name]
                    report.fields_dropped += 1


def _add_field(
    rule: FieldsRule,
    targets: list[PeripheralDesc],
    peripherals: dict[str, PeripheralDesc],
    suffixes: list[str],
    touched: set[tuple[str, str, str, str]],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    assert rule.add is not None
    added = rule.add
    if (
        rule.fields
        or rule.drop
        or rule.rename is not None
        or rule.width is not None
        or rule.on_write is not None
        or rule.on_read is not None
        or rule.acc is not None
    ):
        raise DescriptionError(
            file,
            0,
            f"{key}.add",
            "a rule with no fields, drop, rename, width, on_write, on_read or acc",
        )
    if not rule.source:
        raise DescriptionError(file, 0, f"{key}.source", "a non-empty source")
    if added.reset >> added.width:
        raise DescriptionError(file, 0, f"{key}.add.reset", f"a value of {added.width} bits")
    if rule.instances is None:
        targets = [target for target in targets if target.alias_of is None]
    bits = ((1 << added.width) - 1) << added.lsb
    for peripheral in targets:
        if peripheral.alias_of is not None:
            raise DescriptionError(
                file,
                0,
                f"{key}.instances",
                f"a peripheral that is not an alias, not {peripheral.name}",
            )
        for register_template in rule.registers:
            for suffix in suffixes:
                register_name = register_template.replace("{n}", suffix)
                register = _find_register(
                    peripheral, register_name, file=file, key=f"{key}.registers"
                )
                marker = (peripheral.name, register.name, added.name, "add")
                if marker in touched:
                    raise DescriptionError(
                        file, 0, f"{key}.add.name", f"{added.name} added by no earlier rule"
                    )
                touched.add(marker)
                if any(other.name == added.name for other in register.fields):
                    raise DescriptionError(
                        file,
                        0,
                        f"{key}.add.name",
                        f"a name not used in {register.name}, not {added.name}",
                    )
                if added.lsb + added.width > register.width:
                    raise DescriptionError(
                        file,
                        0,
                        f"{key}.add.width",
                        f"bits inside the {register.width}-bit register",
                    )
                if any(bits & (((1 << f.width) - 1) << f.lsb) for f in register.fields):
                    raise DescriptionError(
                        file, 0, f"{key}.add.lsb", "bits clear of the other fields"
                    )
                new_field = FieldDesc(
                    name=added.name,
                    lsb=added.lsb,
                    width=added.width,
                    acc=added.acc,
                    on_write=WriteEffect.MODIFY,
                    on_read=ReadEffect.NONE,
                )
                for target in peripherals.values():
                    if target is peripheral or target.alias_of == peripheral.name:
                        for same in target.registers:
                            if same.name == register.name:
                                same.fields = sorted(
                                    [*same.fields, replace(new_field)],
                                    key=lambda f: f.lsb,
                                    reverse=True,
                                )
                                same.reset = (same.reset & ~bits) | (added.reset << added.lsb)
                                same.reset_mask |= bits
                report.fields_added += 1


def _rename_fields(
    rule: FieldsRule,
    targets: list[PeripheralDesc],
    peripherals: dict[str, PeripheralDesc],
    suffixes: list[str],
    touched: set[tuple[str, str, str, str]],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    assert rule.rename is not None
    if (
        rule.fields
        or rule.drop
        or rule.width is not None
        or rule.on_write is not None
        or rule.on_read is not None
        or rule.acc is not None
    ):
        raise DescriptionError(
            file, 0, f"{key}.rename", "a rule with no fields, drop, width, on_write, on_read or acc"
        )
    if not rule.source:
        raise DescriptionError(file, 0, f"{key}.source", "a non-empty source")
    if not rule.rename:
        raise DescriptionError(file, 0, f"{key}.rename", "at least one field to rename")
    if rule.instances is None:
        targets = [target for target in targets if target.alias_of is None]
    for peripheral in targets:
        if peripheral.alias_of is not None:
            raise DescriptionError(
                file,
                0,
                f"{key}.instances",
                f"a peripheral that is not an alias, not {peripheral.name}",
            )
        for register_template in rule.registers:
            for suffix in suffixes:
                register_name = register_template.replace("{n}", suffix)
                register = _find_register(
                    peripheral, register_name, file=file, key=f"{key}.registers"
                )
                for old, new in rule.rename.items():
                    marker = (peripheral.name, register.name, old, "rename")
                    if marker in touched:
                        raise DescriptionError(
                            file, 0, f"{key}.rename.{old}", f"{old} renamed by no earlier rule"
                        )
                    touched.add(marker)
                    _find_field(register, old, file=file, key=f"{key}.rename.{old}")
                    if any(other.name == new for other in register.fields):
                        raise DescriptionError(
                            file,
                            0,
                            f"{key}.rename.{old}",
                            f"a name not used in {register.name}, not {new}",
                        )
                    for target in peripherals.values():
                        if target is peripheral or target.alias_of == peripheral.name:
                            for same in target.registers:
                                if same.name == register.name:
                                    same.fields = [
                                        replace(f, name=new) if f.name == old else f
                                        for f in same.fields
                                    ]
                    report.fields_renamed += 1


def _resize_field(
    register: RegisterDesc, resized: FieldDesc, width: int, *, file: str, key: str
) -> None:
    if resized.lsb + width > register.width:
        raise DescriptionError(
            file, 0, f"{key}.width", f"bits inside the {register.width}-bit register"
        )
    bits = ((1 << width) - 1) << resized.lsb
    for other in register.fields:
        if other is not resized and bits & (((1 << other.width) - 1) << other.lsb):
            raise DescriptionError(file, 0, f"{key}.width", "bits clear of the other fields")
    resized.width = width


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


def apply_alternate_drops(
    patches: Sequence[LoadedPatch], peripherals: Sequence[PeripheralDesc]
) -> int:
    """Removes the GPIO alternate functions the `[alternates]` rules of @p patches drop from
    @p peripherals, the ports read from the CubeMX GPIO modes file, and returns how many. A rule
    names a port, a pin, an AF number and a signal. Raises DescriptionError when the port has no
    such entry or the entry was dropped before. Validates every rule before removing any."""
    by_name = {p.name: p for p in peripherals}
    remaining = {
        name: list(p.alternates) for name, p in by_name.items() if p.alternates is not None
    }
    for loaded in patches:
        for j, rule in enumerate(loaded.patch.alternates.drop if loaded.patch.alternates else []):
            entry = AlternateDesc(pin=rule.pin, af=rule.af, signal=rule.signal)
            if entry not in remaining.get(rule.peripheral, []):
                raise DescriptionError(
                    str(loaded.path),
                    0,
                    f"alternates.drop[{j}]",
                    f"an alternate function of {rule.peripheral} listed and not yet dropped",
                )
            remaining[rule.peripheral].remove(entry)
    dropped = 0
    for name, kept in remaining.items():
        peripheral = by_name[name]
        assert peripheral.alternates is not None
        dropped += len(peripheral.alternates) - len(kept)
        peripheral.alternates = kept
    return dropped


def _apply_drops(
    rules: Sequence[tuple[PatchPeripherals, str]],
    peripherals: list[PeripheralDesc],
    report: PatchReport,
) -> None:
    known = {p.name for p in peripherals}
    dropped: set[str] = set()
    renamed: dict[str, str] = {}
    for rule, file in rules:
        if not rule.drop and not rule.renames:
            raise DescriptionError(file, 0, "peripherals", "a drop or a renames entry")
        for name in rule.drop or []:
            if name in dropped:
                raise DescriptionError(file, 0, "peripherals.drop", "each name once")
            if name not in known:
                raise DescriptionError(file, 0, f"peripherals.drop.{name}", "a known peripheral")
            dropped.add(name)
    taken = known - dropped
    for rule, file in rules:
        for old, new in (rule.renames or {}).items():
            key = f"peripherals.renames.{old}"
            if old not in taken or old in renamed:
                raise DescriptionError(file, 0, key, "a known peripheral, renamed once")
            if new in taken or new in renamed.values():
                raise DescriptionError(file, 0, key, "a name no other peripheral has")
            renamed[old] = new
    peripherals[:] = [p for p in peripherals if p.name not in dropped]
    for peripheral in peripherals:
        if peripheral.name in renamed:
            peripheral.name = renamed[peripheral.name]
    report.peripherals_dropped += len(dropped)
    report.peripherals_renamed += len(renamed)


def _apply_core(
    rule: PatchCore, core: CoreDesc, touched: set[str], report: PatchReport, *, file: str
) -> None:
    if rule.reset_clock_hz is not None:
        if "reset_clock_hz" in touched:
            raise DescriptionError(file, 0, "core.reset_clock_hz", "not already patched")
        touched.add("reset_clock_hz")
        core.reset_clock_hz = rule.reset_clock_hz
        report.core_fields_set += 1


_ENGINE_PAGE = 0x400
"""Granularity of a memory region, the emulator's page size: the core refuses anything finer."""


def _apply_memory(
    rule: PatchMemory,
    regions: list[RegionDesc],
    peripherals: Mapping[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
) -> None:
    if not rule.add and not rule.controller:
        raise DescriptionError(file, 0, "memory", "an add or a controller entry")
    for added in rule.add or []:
        key = f"memory.add.{added.name}"
        if added.size == 0 or added.start + added.size > 1 << 32:
            raise DescriptionError(file, 0, key, "a non-empty range inside the 32-bit space")
        if added.start % _ENGINE_PAGE != 0 or added.size % _ENGINE_PAGE != 0:
            raise DescriptionError(file, 0, key, "a start and size in whole 1 KB pages")
        if added.physical is not None and not any(
            region.start <= added.physical
            and added.physical + added.size <= region.start + region.size
            for region in regions
        ):
            raise DescriptionError(file, 0, key, "a physical range inside an existing region")
        for region in regions:
            if region.name == added.name:
                raise DescriptionError(file, 0, key, "a region name not already used")
            if added.start < region.start + region.size and region.start < added.start + added.size:
                raise DescriptionError(file, 0, key, f"a range clear of region {region.name}")
        regions.append(copy.deepcopy(added))
        report.regions_added += 1
    by_name = {region.name: region for region in regions}
    for name, peripheral in (rule.controller or {}).items():
        key = f"memory.controller.{name}"
        region = by_name.get(name)
        if region is None:
            raise DescriptionError(file, 0, key, "an existing region")
        if region.acc.write:
            raise DescriptionError(file, 0, key, "a read-only region")
        if region.controller is not None:
            raise DescriptionError(file, 0, key, "a region without a controller")
        if peripheral not in peripherals or peripherals[peripheral].alias_of is not None:
            raise DescriptionError(
                file, 0, key, "a peripheral of the description that is not an alias"
            )
        region.controller = peripheral
        report.controllers_set += 1


def _add_content(region: RegionDesc, offset: int, data: bytes, *, file: str, key: str) -> None:
    if offset + len(data) > region.size:
        raise DescriptionError(
            file, 0, key, f"an offset leaving {len(data)} bytes inside {region.name}"
        )
    content = region.content or []
    for piece in content:
        if offset < piece.offset + len(piece.data) and piece.offset < offset + len(data):
            raise DescriptionError(file, 0, key, f"bytes clear of the content at {piece.offset:#x}")
    region.content = sorted(
        [*content, RegionContent(offset=offset, data=list(data))], key=lambda piece: piece.offset
    )


_SMPS_SUFFIX = "Q"
_DSI_IP = "DSIHOST"


def is_smps_part(refname: str) -> bool:
    """Whether the part of RefName @p refname has the SMPS step-down converter, a RefName
    ending in `Q`, as ST's ordering codes mark it."""
    return refname.endswith(_SMPS_SUFFIX)


def _package_key(part: DbPart) -> str:
    key = part.package
    if any(ip.name == _DSI_IP for ip in part.ips):
        key += " DSI"
    if is_smps_part(part.ref_name):
        key += " SMPS"
    return key


def _apply_signature(
    rule: PatchSignature,
    regions: list[RegionDesc],
    part: DbPart | None,
    report: PatchReport,
    *,
    file: str,
) -> None:
    region = next((r for r in regions if r.name == rule.region), None)
    if region is None or region.kind is not RegionKind.ROM or region.physical is not None:
        raise DescriptionError(
            file, 0, "signature.region", "a read-only region of the description, not an alias"
        )
    if part is None:
        raise DescriptionError(file, 0, "signature", "the CubeMX part of the description")
    if len(rule.uid_words) != 3 or any(not 0 <= word < 1 << 32 for word in rule.uid_words):
        raise DescriptionError(file, 0, "signature.uid_words", "three 32-bit words")
    for name, value in rule.packages.items():
        if not 0 <= value <= 0x1F:
            raise DescriptionError(file, 0, f"signature.packages.{name}", "a PKG code, 0 to 0x1f")
    flash_kib = part.flash_kib[0]
    if flash_kib >= 1 << 16:
        raise DescriptionError(file, 0, "signature.flash_size", "a flash size below 64 MB")
    words = [
        ("flash_size", rule.flash_size, flash_kib.to_bytes(2, "little")),
        ("uid", rule.uid, b"".join(word.to_bytes(4, "little") for word in rule.uid_words)),
    ]
    code = rule.packages.get(_package_key(part))
    if code is not None:
        words.append(("package", rule.package, code.to_bytes(2, "little")))
    for index, piece in enumerate(rule.content or []):
        if not piece.data or any(not 0 <= byte <= 0xFF for byte in piece.data):
            raise DescriptionError(
                file, 0, f"signature.content[{index}].data", "bytes, at least one"
            )
        words.append((f"content[{index}]", piece.offset, bytes(piece.data)))
    for name, offset, data in words:
        _add_content(region, offset, data, file=file, key=f"signature.{name}")
        report.signature_words += 1


def apply_patches(patches: Sequence[LoadedPatch], target: PatchTarget) -> PatchReport:
    """Applies @p patches to @p target in the fixed order: peripheral drops and renames, IP
    renames, aliases, peripheral sizes, added registers, register resets, fields, cross rules,
    IRQ drops and adds, core, memory regions and their controllers, the device signature. Drops
    come first, so a later rule naming a dropped peripheral fails as naming an unknown one. A
    peripheral rename lands before the aliases, so the alias rule pairs the new name with its
    base, and a later rule names the new name. A peripheral size, an added
    register and a register reset land in the named peripheral and in each of its aliases, so
    fields and cross rules may name the register. A fields rule with `drop` removes its fields
    from the named peripheral and its aliases, and a later rule naming them fails. A fields rule
    with `rename` renames fields in the named peripheral and its aliases, and a later rule names
    the new spelling. A fields rule with `add` adds one field, with its reset value, to the
    named register of the named peripheral and its aliases, and a later rule names it. A fields
    rule with `width`, `on_write`, `on_read` or `acc` sets it on the named fields of the named
    peripheral and each of its aliases, and refuses an alias in `instances`. The report counts
    each field of the peripheral and of each alias per property in `fields_touched`, and each
    base field once in `fields_dropped`, `fields_renamed` and `fields_added`.
    Validates every rule before mutating @p target: an error leaves it exactly as it was."""
    peripherals = copy.deepcopy(target.peripherals)
    irqs = copy.deepcopy(target.irqs)
    core = copy.deepcopy(target.core)
    regions = copy.deepcopy(target.regions)
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

    for loaded in patches:
        for j, rule in enumerate(loaded.patch.peripheral_sizes or []):
            _apply_peripheral_size(
                rule, by_name, report, file=str(loaded.path), key=f"peripheral_sizes[{j}]"
            )

    for loaded in patches:
        for j, rule in enumerate(loaded.patch.registers or []):
            _apply_register_add(rule, by_name, report, file=str(loaded.path), key=f"registers[{j}]")

    resets_touched: set[tuple[str, str]] = set()
    for loaded in patches:
        for j, rule in enumerate(loaded.patch.resets or []):
            key = f"resets[{j}]"
            if (rule.peripheral, rule.register) in resets_touched:
                raise DescriptionError(
                    str(loaded.path), 0, f"{key}.register", "a register no earlier rule resets"
                )
            resets_touched.add((rule.peripheral, rule.register))
            _apply_reset(rule, by_name, report, file=str(loaded.path), key=key)

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

    for loaded in patches:
        if loaded.patch.memory is not None:
            _apply_memory(loaded.patch.memory, regions, by_name, report, file=str(loaded.path))

    for loaded in patches:
        if loaded.patch.signature is not None:
            _apply_signature(
                loaded.patch.signature, regions, target.db_part, report, file=str(loaded.path)
            )

    target.peripherals = peripherals
    target.irqs = irqs
    target.core = core
    target.regions = regions
    return report
