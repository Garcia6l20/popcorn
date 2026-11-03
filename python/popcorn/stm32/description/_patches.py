"""Patch file format, loader and application: peripheral drops, renames and merges, fields, cross
rules, aliases, IP renames, IRQ add/drop, GPIO alternate function drops, core overrides, peripheral
sizes, added registers, added memory regions, region controllers, the device signature. popcorn's
own TOML format, so these dataclasses are the format definition, not a mirror of any C++ or
schema type."""

import copy
import fnmatch
import functools
import re
import tomllib
from collections.abc import Iterator, Mapping, Sequence
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
    RegionAccess,
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
    dies: list[str] | None = None
    flash_kib: list[int] | None = None


@dataclass(slots=True, kw_only=True)
class PatchCore:
    source: str
    reset_clock_hz: int | None = None
    mpu: bool | None = None


@dataclass(slots=True, kw_only=True)
class PatchAliases:
    prefix: str
    offset: int
    source: str | None = None


@dataclass(slots=True, kw_only=True)
class PatchIp:
    renames: dict[str, list[str]] | None = None
    renames_if_present: dict[str, list[str]] | None = None
    versions: dict[str, str] | None = None
    source: str | None = None


@dataclass(slots=True, kw_only=True)
class RegisterDrop:
    peripheral: str
    names: list[str]
    source: str = field(metadata={"min_length": 1})


@dataclass(slots=True, kw_only=True)
class RegisterRename:
    peripherals: list[str]
    renames: dict[str, str]
    source: str = field(metadata={"min_length": 1})


@dataclass(slots=True, kw_only=True)
class PatchSvd:
    name: str


@dataclass(slots=True, kw_only=True)
class PatchPeripherals:
    source: str
    drop: list[str] | None = None
    renames: dict[str, str] | None = None


@dataclass(slots=True, kw_only=True)
class PeripheralMerge:
    into: str
    peripherals: list[str]
    size: int = field(metadata={"minimum": 1, "maximum": 4294967295})
    source: str = field(metadata={"min_length": 1})


@dataclass(slots=True, kw_only=True)
class PatchEeprom:
    start: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    controller: str | None = None


@dataclass(slots=True, kw_only=True)
class PatchAlias:
    name: str = field(metadata={"min_length": 1})
    start: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    region: str = field(metadata={"min_length": 1})


@dataclass(slots=True, kw_only=True)
class PatchMemory:
    source: str
    add: list[RegionDesc] | None = None
    alias: list[PatchAlias] | None = None
    controller: dict[str, str] | None = None
    eeprom: PatchEeprom | None = None
    fill: dict[str, int] | None = None


@dataclass(slots=True, kw_only=True)
class PatchCmsis:
    suffix: str
    source: str


@dataclass(slots=True, kw_only=True)
class PatchSignature:
    region: str
    flash_size: int = field(metadata={"minimum": 0})
    uid: int = field(metadata={"minimum": 0})
    uid_words: list[int]
    source: str
    package: int | None = field(default=None, metadata={"minimum": 0})
    packages: dict[str, int] | None = None
    uid_offsets: list[int] | None = None
    flash_size_codes: dict[str, int] | None = None
    content: list[RegionContent] | None = None


@dataclass(slots=True, kw_only=True)
class IrqAdd:
    number: int
    name: str
    peripheral: str


@dataclass(slots=True, kw_only=True)
class IrqShare:
    name: str
    peripheral: str


@dataclass(slots=True, kw_only=True)
class PatchIrqs:
    drop: list[str] | None = None
    add: list[IrqAdd] | None = None
    share: list[IrqShare] | None = None


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
class PartFact:
    part: str
    source: str = field(metadata={"min_length": 1})
    die: str | None = None
    ram_kib: int | None = field(default=None, metadata={"minimum": 1})


@dataclass(slots=True, kw_only=True)
class Patch:
    format: int = field(default=1, metadata={"const": 1})
    match: PatchMatch
    source: str | None = None
    core: PatchCore | None = None
    aliases: PatchAliases | None = None
    ip: PatchIp | None = None
    peripherals: PatchPeripherals | None = None
    peripheral_merges: list[PeripheralMerge] | None = None
    irqs: PatchIrqs | None = None
    alternates: PatchAlternates | None = None
    svd: PatchSvd | None = None
    fields: list[FieldsRule] | None = None
    cross: list[CrossRuleSpec] | None = None
    peripheral_sizes: list[PeripheralSizeSet] | None = None
    registers: list[RegisterAdd] | None = None
    register_drops: list[RegisterDrop] | None = None
    register_renames: list[RegisterRename] | None = None
    resets: list[ResetSet] | None = None
    memory: PatchMemory | None = None
    signature: PatchSignature | None = None
    cmsis: PatchCmsis | None = None
    part_facts: list[PartFact] | None = None


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
    peripherals_merged: int = 0
    ip_renamed: int = 0
    ip_renames_skipped: int = 0
    ip_versions_set: int = 0
    ip_version_changes: list[tuple[str, str, str]] = field(
        default_factory=list[tuple[str, str, str]]
    )
    registers_dropped: int = 0
    registers_renamed: int = 0
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
    fills_set: int = 0
    signature_words: int = 0


def _matches(match: PatchMatch, part: str, svd_name: str, db_part: DbPart | None) -> bool:
    if not any(fnmatch.fnmatch(part, pattern) for pattern in match.parts):
        return False
    svd_patterns = match.svd or []
    if svd_patterns and not any(fnmatch.fnmatch(svd_name, pattern) for pattern in svd_patterns):
        return False
    die_selected = match.dies is None or (db_part is not None and db_part.die in match.dies)
    flash_selected = match.flash_kib is None or (
        db_part is not None and bool(db_part.flash_kib) and db_part.flash_kib[0] in match.flash_kib
    )
    return die_selected and flash_selected


@dataclass(slots=True, kw_only=True)
class LoadedPatch:
    path: Path
    patch: Patch


@dataclass(slots=True)
class _ParsedFile:
    content: bytes
    data: dict[str, object]
    patch: Patch | None = None
    facts: list[PartFact] | None = None


@functools.lru_cache(maxsize=256)
def parsed_patch(key: Path, content: bytes) -> _ParsedFile:
    """The TOML of the patch file at the resolved path @p key holding @p content, cached on both,
    256 files, least recently used dropped first. tomllib.TOMLDecodeError for bytes that are not
    TOML, raised again at every call."""
    text = content.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    return _ParsedFile(content=content, data=tomllib.loads(text))


def _parsed(path: Path) -> _ParsedFile:
    try:
        return parsed_patch(path.resolve(), path.read_bytes())
    except tomllib.TOMLDecodeError as exc:
        raise DescriptionError(str(path), exc.lineno or 0, "<root>", "valid TOML") from exc


def _patch_of(path: Path, entry: _ParsedFile) -> Patch:
    if entry.patch is None:
        entry.patch = from_mapping(Patch, entry.data, file=str(path))
    return entry.patch


def read_patch(path: Path) -> Patch:
    """The patch in the file @p path, parsed once per process: an entry is keyed on the resolved
    path and reused while the file's bytes equal the bytes it was parsed from, so an edited file
    is parsed again at its next read. At most 256 files are kept, see parsed_patch.
    DescriptionError for a file that is not TOML or not a patch."""
    return _patch_of(path, _parsed(path))


def patch_contents(files: Sequence[Path]) -> tuple[tuple[Path, bytes], ...]:
    """The resolved path and the bytes of each of @p files, read and parsed as read_patch reads
    them, in the order of @p files. DescriptionError for a file that is not TOML."""
    result: list[tuple[Path, bytes]] = []
    for path in files:
        result.append((path.resolve(), _parsed(path).content))
    return tuple(result)


def load_patches(
    files: Sequence[Path], part: str, svd_name: str, db_part: DbPart | None = None
) -> list[LoadedPatch]:
    """Patches whose [match] selects @p part and @p svd_name, in the order @p files lists
    them (family files before part files is the caller's ordering of @p files). A patch with
    `dies` or `flash_kib` in its [match] applies only when @p db_part, the selected expanded
    name's part, is given and its die is listed and its flash size is listed. Each file is read
    through read_patch."""
    result: list[LoadedPatch] = []
    for path in files:
        patch = read_patch(path)
        if _matches(patch.match, part, svd_name, db_part):
            result.append(LoadedPatch(path=path, patch=patch))
    return result


def _checked_facts(path: Path, entry: _ParsedFile) -> list[PartFact]:
    if "part_facts" not in entry.data:
        return []
    patch = _patch_of(path, entry)
    file = str(path)
    match = patch.match
    if match.svd is not None or match.dies is not None or match.flash_kib is not None:
        raise DescriptionError(file, 0, "match", "parts only in a patch with part_facts")
    facts = patch.part_facts or []
    for index, fact in enumerate(facts):
        key = f"part_facts[{index}]"
        if not any(fnmatch.fnmatch(fact.part, pattern) for pattern in match.parts):
            raise DescriptionError(file, 0, f"{key}.part", "a part the [match] selects")
        if fact.die is None and fact.ram_kib is None:
            raise DescriptionError(file, 0, key, "a die or a ram_kib")
    return facts


def _part_facts_of(path: Path) -> list[PartFact]:
    entry = _parsed(path)
    if entry.facts is None:
        entry.facts = _checked_facts(path, entry)
    return entry.facts


def part_facts(files: Sequence[Path]) -> dict[str, PartFact]:
    """The `[[part_facts]]` rows of @p files by part name, the corrections of the db part facts
    that a manual or a datasheet states otherwise. Each file is parsed as read_patch parses it.
    DescriptionError for a patch with part_facts whose [match] has more than `parts`, a row
    whose part the [match] does not select, a row with neither `die` nor `ram_kib`, and a second
    row for one part."""
    found: dict[str, PartFact] = {}
    owners: dict[str, str] = {}
    for path in files:
        for index, fact in enumerate(_part_facts_of(path)):
            if fact.part in found:
                raise DescriptionError(
                    str(path),
                    0,
                    f"part_facts[{index}].part",
                    f"one row per part, {owners[fact.part]} has one",
                )
            found[fact.part] = fact
            owners[fact.part] = str(path)
    return found


def with_part_facts(part: DbPart, facts: Mapping[str, PartFact]) -> DbPart:
    """@p part, a selected expanded name's part, with the die and the SRAM size of its row in
    @p facts, a part_facts result. @p part itself without a row."""
    fact = facts.get(part.ref_name)
    if fact is None:
        return part
    return replace(
        part,
        die=fact.die if fact.die is not None else part.die,
        ram_kib=[fact.ram_kib] if fact.ram_kib is not None else part.ram_kib,
    )


def corrected_part(part: DbPart, files: Sequence[Path]) -> DbPart:
    """@p part, a selected expanded name's part, with the die and the SRAM size of its
    `[[part_facts]]` row in @p files, see part_facts and with_part_facts."""
    return with_part_facts(part, part_facts(files))


_CMSIS_SUFFIX_RE = re.compile(r"x[a-z0-9]+")


def cmsis_suffix(patches: Sequence[LoadedPatch]) -> str | None:
    """The `[cmsis] suffix` of the one patch in @p patches that sets it, None when none does.
    DescriptionError for a suffix that is not `x` followed by lowercase letters and digits, and
    for a second patch setting it."""
    found: tuple[str, str] | None = None
    for loaded in patches:
        rule = loaded.patch.cmsis
        if rule is None:
            continue
        file = str(loaded.path)
        if _CMSIS_SUFFIX_RE.fullmatch(rule.suffix) is None:
            raise DescriptionError(
                file, 0, "cmsis.suffix", "x followed by lowercase letters or digits"
            )
        if not rule.source.strip():
            raise DescriptionError(file, 0, "cmsis.source", "a non-empty source")
        if found is not None:
            raise DescriptionError(file, 0, "cmsis", f"no other patch setting it, {found[1]} does")
        found = (rule.suffix, file)
    return found[0] if found is not None else None


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
    if rule.renames is None and rule.renames_if_present is None and rule.versions is None:
        raise DescriptionError(file, 0, "ip", "a renames, renames_if_present or versions entry")
    for db_instance, targets in (rule.renames or {}).items():
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
    for db_instance, targets in (rule.renames_if_present or {}).items():
        db_ip = db_ips.get(db_instance)
        for target_name in targets:
            peripheral = peripherals.get(target_name)
            if db_ip is None or peripheral is None:
                report.ip_renames_skipped += 1
                continue
            peripheral.ip = db_ip.name
            peripheral.ip_version = db_ip.version
            report.ip_renamed += 1


def _apply_ip_versions(
    rule: PatchIp,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
) -> None:
    if rule.versions is None:
        return
    if not rule.source:
        raise DescriptionError(file, 0, "ip.source", "a non-empty source")
    set_here: set[str] = set()
    for pattern, version in rule.versions.items():
        key = f"ip.versions.{pattern}"
        matched = [p for name, p in peripherals.items() if fnmatch.fnmatchcase(name, pattern)]
        if not matched:
            raise DescriptionError(file, 0, key, "an instance name matching the glob")
        for peripheral in matched:
            if peripheral.name in set_here:
                raise DescriptionError(file, 0, key, "instances no other glob of the table sets")
            set_here.add(peripheral.name)
            report.ip_version_changes.append((peripheral.name, peripheral.ip_version, version))
            peripheral.ip_version = version
            report.ip_versions_set += 1


def _peripheral(
    name: str, peripherals: dict[str, PeripheralDesc], *, file: str, key: str
) -> PeripheralDesc:
    peripheral = peripherals.get(name)
    if peripheral is None or peripheral.alias_of is not None:
        raise DescriptionError(
            file, 0, f"{key}.peripheral", "a peripheral of the description that is not an alias"
        )
    return peripheral


def _direct_targets(
    instances: list[str] | None, targets: list[PeripheralDesc], *, file: str, key: str
) -> Iterator[PeripheralDesc]:
    if instances is None:
        targets = [target for target in targets if target.alias_of is None]
    for peripheral in targets:
        if peripheral.alias_of is not None:
            raise DescriptionError(
                file,
                0,
                f"{key}.instances",
                f"a peripheral that is not an alias, not {peripheral.name}",
            )
        yield peripheral


def _apply_register_drops(
    rule: RegisterDrop,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    peripheral = _peripheral(rule.peripheral, peripherals, file=file, key=key)
    if not rule.names:
        raise DescriptionError(file, 0, f"{key}.names", "at least one register name")
    present = {r.name for r in peripheral.registers}
    seen: set[str] = set()
    for name in rule.names:
        if name not in present:
            raise DescriptionError(
                file, 0, f"{key}.names", f"a register named {name} in {peripheral.name}"
            )
        if name in seen:
            raise DescriptionError(file, 0, f"{key}.names", "each name once")
        seen.add(name)
    for register in peripheral.registers:
        if register.alternate_of in seen and register.name not in seen:
            raise DescriptionError(
                file,
                0,
                f"{key}.names",
                f"{register.alternate_of} dropped with its alternate {register.name}",
            )
    for target in peripherals.values():
        if target is peripheral or target.alias_of == peripheral.name:
            target.registers = [r for r in target.registers if r.name not in seen]
    report.registers_dropped += len(seen)


def _apply_register_renames(
    rule: RegisterRename,
    peripherals: dict[str, PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    if not rule.renames:
        raise DescriptionError(file, 0, f"{key}.renames", "at least one register to rename")
    targets: list[PeripheralDesc] = []
    for pattern in rule.peripherals:
        named = [p for p in peripherals.values() if fnmatch.fnmatchcase(p.name, pattern)]
        matched = [p for p in named if p.alias_of is None]
        if not matched:
            expected = (
                f"a peripheral that is not an alias matching {pattern}"
                if named
                else f"a peripheral matching {pattern}"
            )
            raise DescriptionError(file, 0, f"{key}.peripherals", expected)
        targets.extend(p for p in matched if all(p is not t for t in targets))
    if not targets:
        raise DescriptionError(file, 0, f"{key}.peripherals", "at least one peripheral glob")
    for target in targets:
        present = {r.name for r in target.registers}
        for old, new in rule.renames.items():
            if old not in present:
                raise DescriptionError(
                    file, 0, f"{key}.renames.{old}", f"a register named {old} in {target.name}"
                )
            if new in present:
                raise DescriptionError(
                    file, 0, f"{key}.renames.{old}", f"a name not used in {target.name}, not {new}"
                )
    names = {t.name for t in targets}
    for peripheral in peripherals.values():
        if peripheral.name in names or peripheral.alias_of in names:
            for register in peripheral.registers:
                register.name = rule.renames.get(register.name, register.name)
                if register.alternate_of is not None:
                    register.alternate_of = rule.renames.get(
                        register.alternate_of, register.alternate_of
                    )
    report.registers_renamed += len(rule.renames) * len(targets)


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
    peripheral = _peripheral(rule.peripheral, peripherals, file=file, key=key)
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
    peripheral = _peripheral(rule.peripheral, peripherals, file=file, key=key)
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
    peripheral = _peripheral(rule.peripheral, peripherals, file=file, key=key)
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
    for peripheral in _direct_targets(rule.instances, targets, file=file, key=key):
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
    for peripheral in _direct_targets(rule.instances, targets, file=file, key=key):
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
    bits = ((1 << added.width) - 1) << added.lsb
    for peripheral in _direct_targets(rule.instances, targets, file=file, key=key):
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
    for peripheral in _direct_targets(rule.instances, targets, file=file, key=key):
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


def _end(register: RegisterDesc) -> int:
    return register.offset + max(register.width // 8, 1)


def _overlap(first: RegisterDesc, second: RegisterDesc) -> bool:
    return first.offset < _end(second) and second.offset < _end(first)


def _apply_peripheral_merge(
    rule: PeripheralMerge,
    peripherals: list[PeripheralDesc],
    report: PatchReport,
    *,
    file: str,
    key: str,
) -> None:
    by_name = {p.name: p for p in peripherals}
    into = by_name.get(rule.into)
    if into is None:
        raise DescriptionError(file, 0, f"{key}.into", "a peripheral of the description")
    if not rule.peripherals:
        raise DescriptionError(file, 0, f"{key}.peripherals", "at least one peripheral to merge")
    if rule.size < into.size:
        raise DescriptionError(file, 0, f"{key}.size", f"a size of at least {into.size:#x}")
    groups: list[list[RegisterDesc]] = [into.registers]
    irqs = list(into.irqs)
    merged: set[str] = set()
    for name in rule.peripherals:
        part_key = f"{key}.peripherals.{name}"
        source = by_name.get(name)
        if source is None:
            raise DescriptionError(file, 0, part_key, "a peripheral of the description")
        if source is into or name in merged:
            raise DescriptionError(
                file, 0, part_key, f"each peripheral once, other than {into.name}"
            )
        if source.base < into.base:
            raise DescriptionError(file, 0, part_key, f"a base at or above {into.base:#x}")
        delta = source.base - into.base
        moved = [replace(copy.deepcopy(r), offset=r.offset + delta) for r in source.registers]
        groups.append(moved)
        merged.add(name)
        irqs.extend(number for number in source.irqs if number not in irqs)
    names = {r.name for r in into.registers}
    for index, group in enumerate(groups[1:], start=1):
        for register in group:
            part_key = f"{key}.peripherals.{rule.peripherals[index - 1]}"
            if register.name in names:
                raise DescriptionError(
                    file, 0, part_key, f"register names not in use, not {register.name}"
                )
            names.add(register.name)
            if _end(register) > rule.size:
                raise DescriptionError(
                    file,
                    0,
                    f"{key}.size",
                    f"a size covering {register.name} at {register.offset:#x}",
                )
            for other in (r for earlier in groups[:index] for r in earlier):
                if _overlap(register, other):
                    raise DescriptionError(
                        file, 0, part_key, f"{register.name} clear of {other.name}"
                    )
    into.registers = sorted([r for group in groups for r in group], key=lambda r: r.offset)
    into.irqs = irqs
    into.size = rule.size
    peripherals[:] = [p for p in peripherals if p.name not in merged]
    report.peripherals_merged += len(merged)


def peripheral_owners(patches: Sequence[LoadedPatch]) -> dict[str, str | None]:
    """Maps each SVD peripheral name that a `[peripherals]` drop or rename or a
    `[[peripheral_merges]]` rule of @p patches touches to the name of the peripheral holding it
    once they apply, None for a dropped one. A name the map lacks keeps its own name. Assumes
    @p patches applied without error."""
    owners: dict[str, str | None] = {}
    for loaded in patches:
        rule = loaded.patch.peripherals
        if rule is not None:
            owners.update(dict.fromkeys(rule.drop or []))
            owners.update(rule.renames or {})
    renamed = set(owners.values())
    for loaded in patches:
        for merge in loaded.patch.peripheral_merges or []:
            for name in merge.peripherals:
                held = [svd_name for svd_name, owner in owners.items() if owner == name]
                if name not in owners and name not in renamed:
                    held.append(name)
                owners.update(dict.fromkeys(held, merge.into))
    return owners


def _apply_core(
    rule: PatchCore, core: CoreDesc, touched: set[str], report: PatchReport, *, file: str
) -> None:
    if rule.reset_clock_hz is not None:
        if "reset_clock_hz" in touched:
            raise DescriptionError(file, 0, "core.reset_clock_hz", "not already patched")
        touched.add("reset_clock_hz")
        core.reset_clock_hz = rule.reset_clock_hz
        report.core_fields_set += 1
    if rule.mpu is not None:
        if "mpu" in touched:
            raise DescriptionError(file, 0, "core.mpu", "not already patched")
        touched.add("mpu")
        core.mpu = rule.mpu
        report.core_fields_set += 1


_ENGINE_PAGE = 0x400
"""Granularity of a memory region, the emulator's page size: the core refuses anything finer."""


_BIT_BAND_SIZE = 32 << 20
"""Size of a Cortex-M bit-band alias window."""

_BIT_BAND_PAIRS = {0x2200_0000: 0x2000_0000, 0x4200_0000: 0x4000_0000}
"""The architectural windows, alias start onto the target's first byte, DDI0337H 3.7: the lowest
1 MB of the SRAM region and of the peripheral region."""


def _check_bit_band(added: RegionDesc, key: str, *, file: str) -> None:
    """DescriptionError unless `added` is one of the two architectural 32 MB windows, with no
    content and no controller."""
    if added.size != _BIT_BAND_SIZE:
        raise DescriptionError(file, 0, key, "a bit_band region of 32 MB")
    if added.start not in _BIT_BAND_PAIRS or added.physical != _BIT_BAND_PAIRS[added.start]:
        raise DescriptionError(
            file,
            0,
            key,
            "a bit_band window at 0x22000000 onto 0x20000000 or at 0x42000000 onto 0x40000000",
        )
    if added.content is not None or added.controller is not None:
        raise DescriptionError(file, 0, key, "a bit_band region with no content and no controller")


_EEPROM_REGION = "DATA_EEPROM"


def _add_region(
    added: RegionDesc, regions: list[RegionDesc], report: PatchReport, *, file: str, key: str
) -> None:
    if added.size == 0 or added.start + added.size > 1 << 32:
        raise DescriptionError(file, 0, key, "a non-empty range inside the 32-bit space")
    if added.start % _ENGINE_PAGE != 0 or added.size % _ENGINE_PAGE != 0:
        raise DescriptionError(file, 0, key, "a start and size in whole 1 KB pages")
    if added.kind is RegionKind.BIT_BAND:
        _check_bit_band(added, key, file=file)
    elif added.physical is not None and not any(
        region.start <= added.physical and added.physical + added.size <= region.start + region.size
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


def _add_alias(
    alias: PatchAlias, regions: list[RegionDesc], report: PatchReport, *, file: str
) -> None:
    key = f"memory.alias.{alias.name}"
    target = next((region for region in regions if region.name == alias.region), None)
    if target is None:
        raise DescriptionError(file, 0, key, "an existing region")
    if target.kind is RegionKind.BIT_BAND or target.physical is not None:
        raise DescriptionError(file, 0, key, "a region that is neither bit_band nor an alias")
    _add_region(
        RegionDesc(
            name=alias.name,
            start=alias.start,
            size=target.size,
            kind=target.kind,
            acc=target.acc,
            secure=target.secure,
            physical=target.start,
        ),
        regions,
        report,
        file=file,
        key=key,
    )


def _apply_memory(
    rule: PatchMemory,
    regions: list[RegionDesc],
    peripherals: Mapping[str, PeripheralDesc],
    part: DbPart | None,
    report: PatchReport,
    *,
    file: str,
) -> None:
    if (
        not rule.add
        and not rule.alias
        and not rule.controller
        and rule.eeprom is None
        and not rule.fill
    ):
        raise DescriptionError(file, 0, "memory", "an add, alias, eeprom, controller or fill entry")
    for added in rule.add or []:
        _add_region(added, regions, report, file=file, key=f"memory.add.{added.name}")
    for alias in rule.alias or []:
        _add_alias(alias, regions, report, file=file)
    controllers = dict(rule.controller or {})
    if rule.eeprom is not None:
        key = "memory.eeprom"
        size = part.eeprom_bytes if part is not None else None
        if size is None:
            raise DescriptionError(file, 0, key, "a part with an E2prom element in the CubeMX db")
        if _EEPROM_REGION in controllers:
            raise DescriptionError(file, 0, key, f"no controller entry for {_EEPROM_REGION}")
        _add_region(
            RegionDesc(
                name=_EEPROM_REGION,
                start=rule.eeprom.start,
                size=size,
                kind=RegionKind.ROM,
                acc=RegionAccess(read=True, write=False, execute=False),
                secure=False,
            ),
            regions,
            report,
            file=file,
            key=key,
        )
        if rule.eeprom.controller is not None:
            controllers[_EEPROM_REGION] = rule.eeprom.controller
    by_name = {region.name: region for region in regions}
    for name, peripheral in controllers.items():
        key = f"memory.controller.{name}"
        region = by_name.get(name)
        if region is None:
            raise DescriptionError(file, 0, key, "an existing region")
        if region.kind is RegionKind.BIT_BAND:
            raise DescriptionError(file, 0, key, "a region of a kind other than bit_band")
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
    for name, value in (rule.fill or {}).items():
        key = f"memory.fill.{name}"
        region = by_name.get(name)
        if region is None:
            raise DescriptionError(file, 0, key, "an existing region")
        if region.kind is not RegionKind.ROM or region.physical is not None:
            raise DescriptionError(file, 0, key, "a rom region that is not an alias")
        if not 0 <= value <= 0xFF:
            raise DescriptionError(file, 0, key, "a byte value, 0 to 255")
        region.fill = value
        report.fills_set += 1


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


_UID_OFFSETS = [0, 4, 8]


def _uid_pieces(base: int, words: Sequence[int], offsets: Sequence[int]) -> list[tuple[int, bytes]]:
    """The 32-bit @p words at @p base plus their @p offsets as content pieces, little endian, words
    that follow each other in one piece."""
    pieces: list[tuple[int, bytes]] = []
    for offset, word in sorted(zip(offsets, words, strict=True)):
        data = word.to_bytes(4, "little")
        if pieces and pieces[-1][0] + len(pieces[-1][1]) == base + offset:
            pieces[-1] = (pieces[-1][0], pieces[-1][1] + data)
        else:
            pieces.append((base + offset, data))
    return pieces


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
    if (rule.package is None) != (rule.packages is None):
        raise DescriptionError(file, 0, "signature.packages", "packages with package, or neither")
    offsets = _UID_OFFSETS if rule.uid_offsets is None else rule.uid_offsets
    if len(offsets) != 3 or any(not 0 <= offset < 1 << 16 for offset in offsets):
        raise DescriptionError(file, 0, "signature.uid_offsets", "three offsets, 0 to 0xffff")
    for name, value in (rule.packages or {}).items():
        if not 0 <= value <= 0x1F:
            raise DescriptionError(file, 0, f"signature.packages.{name}", "a PKG code, 0 to 0x1f")
    flash_kib = part.flash_kib[0]
    if flash_kib >= 1 << 16:
        raise DescriptionError(file, 0, "signature.flash_size", "a flash size below 64 MB")
    size_word = flash_kib
    if rule.flash_size_codes is not None:
        for kib, code in rule.flash_size_codes.items():
            if not kib.isdecimal() or not 0 <= code < 1 << 16:
                raise DescriptionError(
                    file,
                    0,
                    f"signature.flash_size_codes.{kib}",
                    "a flash size in KiB mapped to a 16-bit code",
                )
        if str(flash_kib) not in rule.flash_size_codes:
            raise DescriptionError(
                file, 0, "signature.flash_size_codes", f"a code for a flash of {flash_kib} KiB"
            )
        size_word = rule.flash_size_codes[str(flash_kib)]
    uid_pieces = _uid_pieces(rule.uid, rule.uid_words, offsets)
    words = [
        ("flash_size", [(rule.flash_size, size_word.to_bytes(2, "little"))]),
        ("uid", uid_pieces),
    ]
    if rule.package is not None and rule.packages is not None:
        code = rule.packages.get(_package_key(part))
        if code is not None:
            words.append(("package", [(rule.package, code.to_bytes(2, "little"))]))
    for index, piece in enumerate(rule.content or []):
        if not piece.data or any(not 0 <= byte <= 0xFF for byte in piece.data):
            raise DescriptionError(
                file, 0, f"signature.content[{index}].data", "bytes, at least one"
            )
        words.append((f"content[{index}]", [(piece.offset, bytes(piece.data))]))
    for name, pieces in words:
        for offset, data in pieces:
            _add_content(region, offset, data, file=file, key=f"signature.{name}")
        report.signature_words += 1


def apply_patches(patches: Sequence[LoadedPatch], target: PatchTarget) -> PatchReport:
    """Applies @p patches to @p target in the fixed order: peripheral drops and renames, then per
    patch in file order the IP renames and `renames_if_present`, then the `versions` of every patch
    in file order, aliases, register drops, register renames, peripheral sizes, added registers,
    register resets, fields, cross rules, IRQ drops and adds, core, memory regions and their
    controllers, the device signature. Drops come first, so a later rule naming a dropped peripheral
    fails as naming an unknown one. An `[ip] versions` glob matches the instance names after every
    `[peripherals] renames`, and a glob matching nothing is an error. A peripheral rename lands
    before the aliases, so the alias rule pairs the new name with its base, and a later rule names
    the new name. Peripheral merges run next, in file order. A merge names its `into` and merged
    peripherals as the drops and renames leave them, moves the registers of each merged peripheral
    into `into` at their offset plus the distance between the two bases, moves their IRQs, removes
    them and sets the size of `into`, so a later rule names `into` only.
    A peripheral size, an added register and a register reset land in the named
    peripheral and in each of its aliases, so fields and cross rules may name the register. A fields
    rule with `drop` removes its fields from the named peripheral and its aliases, and a later rule
    naming them fails. A fields rule with `rename` renames fields in the named peripheral and its
    aliases, and a later rule names the new spelling. A fields rule with `add` adds one field, with
    its reset value, to the named register of the named peripheral and its aliases, and a later rule
    names it. A fields rule with `width`, `on_write`, `on_read` or `acc` sets it on the named fields
    of the named peripheral and each of its aliases, and refuses an alias in `instances`. The report
    counts each field of the peripheral and of each alias per property in `fields_touched`, and each
    base field once in `fields_dropped`, `fields_renamed` and `fields_added`. Validates every rule
    before mutating @p target: an error leaves it exactly as it was."""
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

    for loaded in patches:
        for j, rule in enumerate(loaded.patch.peripheral_merges or []):
            _apply_peripheral_merge(
                rule, peripherals, report, file=str(loaded.path), key=f"peripheral_merges[{j}]"
            )

    by_name = {p.name: p for p in peripherals}

    for loaded in patches:
        if loaded.patch.ip is not None:
            _apply_ip_renames(
                loaded.patch.ip, by_name, target.db_ips, report, file=str(loaded.path)
            )

    for loaded in patches:
        if loaded.patch.ip is not None:
            _apply_ip_versions(loaded.patch.ip, by_name, report, file=str(loaded.path))

    for loaded in patches:
        if loaded.patch.aliases is not None:
            _apply_aliases(loaded.patch.aliases, by_name, report, file=str(loaded.path))

    for loaded in patches:
        for j, rule in enumerate(loaded.patch.register_drops or []):
            _apply_register_drops(
                rule, by_name, report, file=str(loaded.path), key=f"register_drops[{j}]"
            )

    for loaded in patches:
        for j, rule in enumerate(loaded.patch.register_renames or []):
            _apply_register_renames(
                rule, by_name, report, file=str(loaded.path), key=f"register_renames[{j}]"
            )

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
            _apply_memory(
                loaded.patch.memory,
                regions,
                by_name,
                target.db_part,
                report,
                file=str(loaded.path),
            )

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
