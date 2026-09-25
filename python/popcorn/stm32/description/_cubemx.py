"""CubeMX db: part lookup, rzone regions and processor facts, IP instance join."""

import xml.etree.ElementTree as ET
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from xml.etree.ElementTree import Element

from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import RegionAccess, RegionDesc, RegionKind

_MCU_NS = "http://mcd.rou.st.com/modules.php?name=mcu"


@dataclass(slots=True, frozen=True, kw_only=True)
class DbIp:
    instance: str
    name: str
    version: str


@dataclass(slots=True, frozen=True, kw_only=True)
class DbPart:
    ref_name: str
    family: str
    line: str
    die: str
    ram_kib: list[int]
    flash_kib: list[int]
    ips: list[DbIp]


@dataclass(slots=True, frozen=True, kw_only=True)
class RzoneProcessor:
    clock_hz: int
    core: str
    core_version: str
    fpu: str | None
    dsp: bool
    mpu: bool
    tz: bool
    num_interrupts: int
    mpu_regions: int
    sec_mpu_regions: int
    sau_regions: int


@dataclass(slots=True, frozen=True, kw_only=True)
class Rzone:
    processor: RzoneProcessor
    regions: list[RegionDesc]


def _text(el: Element, tag: str) -> str | None:
    child = el.find(tag)
    if child is None or child.text is None:
        return None
    text = child.text.strip()
    return text or None


def read_part(data: bytes, *, file: str) -> DbPart:
    """DbPart of a `db/mcu/<part>.xml` document, DescriptionError when it is malformed or does
    not carry the CubeMX Mcu namespace."""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        line = exc.position[0] if exc.position else 0
        raise DescriptionError(file, line, "<root>", "valid XML") from exc
    if not root.tag.startswith("{"):
        raise DescriptionError(file, 0, "Mcu", f"namespace {_MCU_NS}")
    uri = root.tag[1:].split("}", 1)[0]
    if uri != _MCU_NS:
        raise DescriptionError(file, 0, "Mcu", f"namespace {_MCU_NS}")

    def q(tag: str) -> str:
        return f"{{{uri}}}{tag}"

    ref_name = root.get("RefName")
    if ref_name is None:
        raise DescriptionError(file, 0, "Mcu.RefName", "required")
    family = root.get("Family") or ""
    line = root.get("Line") or ""

    die = _text(root, q("Die"))
    if die is None:
        raise DescriptionError(file, 0, "Mcu.Die", "required")

    ram_kib = [_parse_kib(e, file=file, key="Mcu.Ram") for e in root.findall(q("Ram"))]
    if not ram_kib:
        raise DescriptionError(file, 0, "Mcu.Ram", "required")
    flash_kib = [_parse_kib(e, file=file, key="Mcu.Flash") for e in root.findall(q("Flash"))]
    if not flash_kib:
        raise DescriptionError(file, 0, "Mcu.Flash", "required")

    ips: list[DbIp] = []
    for ip_el in root.findall(q("IP")):
        instance = ip_el.get("InstanceName")
        name = ip_el.get("Name")
        if instance is None or name is None:
            raise DescriptionError(file, 0, "Mcu.IP", "InstanceName and Name")
        ips.append(DbIp(instance=instance, name=name, version=ip_el.get("Version") or ""))

    return DbPart(
        ref_name=ref_name,
        family=family,
        line=line,
        die=die,
        ram_kib=ram_kib,
        flash_kib=flash_kib,
        ips=ips,
    )


def _parse_kib(el: Element, *, file: str, key: str) -> int:
    text = (el.text or "").strip()
    try:
        return int(text)
    except ValueError as exc:
        raise DescriptionError(file, 0, key, "integer") from exc


def rzone_file_name(part: DbPart) -> str:
    """Name of the `db/mcu/memory/` rzone file for @p part, first Ram and Flash element."""
    return f"STM32_{part.die}_{part.ram_kib[0]}_{part.flash_kib[0]}.xml"


def _int_attr(el: Element, name: str, *, file: str, key: str) -> int:
    text = el.get(name)
    if text is None:
        raise DescriptionError(file, 0, key, "required")
    try:
        return int(text)
    except ValueError as exc:
        raise DescriptionError(file, 0, key, "integer") from exc


def _int_attr_or(el: Element, name: str, default: int, *, file: str, key: str) -> int:
    if el.get(name) is None:
        return default
    return _int_attr(el, name, file=file, key=key)


def _parse_processor(el: Element, *, file: str) -> RzoneProcessor:
    return RzoneProcessor(
        clock_hz=_int_attr(el, "Dclock", file=file, key="processor.Dclock"),
        core=el.get("Dcore") or "",
        core_version=el.get("DcoreVersion") or "",
        fpu=el.get("Dfpu"),
        dsp=el.get("Ddsp") == "DSP",
        mpu=el.get("Dmpu") == "MPU",
        tz=el.get("Dtz") == "TZ",
        num_interrupts=_int_attr(el, "DnumInterrupts", file=file, key="processor.DnumInterrupts"),
        mpu_regions=_int_attr_or(
            el, "DnumMpuRegions", 0, file=file, key="processor.DnumMpuRegions"
        ),
        sec_mpu_regions=_int_attr_or(
            el, "DnumSecMpuRegions", 0, file=file, key="processor.DnumSecMpuRegions"
        ),
        sau_regions=_int_attr_or(
            el, "DnumSauRegions", 0, file=file, key="processor.DnumSauRegions"
        ),
    )


def _parse_region(el: Element, *, file: str) -> RegionDesc:
    name = el.get("name")
    if name is None:
        raise DescriptionError(file, 0, "memory.name", "required")
    start_text = el.get("start")
    size_text = el.get("size")
    if start_text is None or size_text is None:
        raise DescriptionError(file, 0, f"memory[{name}]", "start and size")
    try:
        start = int(start_text, 16)
        size = int(size_text, 16)
    except ValueError as exc:
        raise DescriptionError(file, 0, f"memory[{name}]", "hex integer") from exc

    type_text = el.get("type")
    if type_text is None:
        raise DescriptionError(file, 0, f"memory[{name}].type", "required")
    try:
        kind = RegionKind(type_text.lower())
    except ValueError:
        valid = ", ".join(member.value for member in RegionKind)
        raise DescriptionError(file, 0, f"memory[{name}].type", f"one of {valid}") from None

    access_text = el.get("access") or ""
    acc = RegionAccess(
        read="r" in access_text, write="w" in access_text, execute="x" in access_text
    )
    secure = (el.get("security") or "n") != "n"
    physical_text = el.get("physical")
    try:
        physical = int(physical_text, 16) if physical_text else None
    except ValueError as exc:
        raise DescriptionError(file, 0, f"memory[{name}].physical", "hex integer") from exc

    return RegionDesc(
        name=name, start=start, size=size, kind=kind, acc=acc, secure=secure, physical=physical
    )


def read_rzone(data: bytes, *, file: str) -> Rzone:
    """Rzone of a `db/mcu/memory/STM32_<Die>_<Ram>_<Flash>.xml` document, DescriptionError when
    it is malformed or names an unsupported memory type."""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        line = exc.position[0] if exc.position else 0
        raise DescriptionError(file, line, "<root>", "valid XML") from exc
    processor_el = root.find(".//processor")
    if processor_el is None:
        raise DescriptionError(file, 0, "processor", "required")
    processor = _parse_processor(processor_el, file=file)
    regions = [_parse_region(el, file=file) for el in root.iter("memory")]
    return Rzone(processor=processor, regions=regions)


def instance_ips(
    part: DbPart, svd_names: Iterable[str], renames: Mapping[str, Sequence[str]]
) -> dict[str, DbIp]:
    """SVD peripheral name -> matched DbIp: exact instance-name match first, then @p renames
    (db instance name -> the SVD peripheral names it covers). Unmatched names are absent."""
    by_instance = {ip.instance: ip for ip in part.ips}
    names = set(svd_names)
    result: dict[str, DbIp] = {}
    for name in names:
        ip = by_instance.get(name)
        if ip is not None:
            result[name] = ip
    for db_instance, targets in renames.items():
        ip = by_instance.get(db_instance)
        if ip is None:
            continue
        for target in targets:
            if target in names and target not in result:
                result[target] = ip
    return result
