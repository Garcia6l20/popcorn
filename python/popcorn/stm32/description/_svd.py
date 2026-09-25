"""SVD expander: derivedFrom, dim, clusters, property inheritance, no interrupt copy.

Pure function of the file bytes: ``expand_svd`` never touches the filesystem.
"""

import copy
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from enum import StrEnum
from xml.etree.ElementTree import Element

from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    FieldDesc,
    ReadEffect,
    RegisterDesc,
    WriteEffect,
)


@dataclass(slots=True, frozen=True, kw_only=True)
class SvdCpu:
    name: str = ""
    revision: str = ""
    fpu_present: bool = False
    fpu_dp: bool = False
    mpu_present: bool = False
    dsp_present: bool = False
    nvic_prio_bits: int = 0
    vendor_systick_config: bool = False
    sau_num_regions: int = 0


@dataclass(slots=True, frozen=True, kw_only=True)
class SvdPeripheral:
    name: str
    base: int
    size: int
    group: str
    derived_from: str | None
    registers: list[RegisterDesc]
    interrupts: list[tuple[int, str]]


@dataclass(slots=True, frozen=True, kw_only=True)
class SvdDevice:
    name: str
    version: str
    cpu: SvdCpu
    peripherals: list[SvdPeripheral]

    def unlinked_overlaps(self) -> list[tuple[str, str, str]]:
        """Peripheral, anchor register and other register sharing an offset with neither
        carrying `alternate_of`."""
        result: list[tuple[str, str, str]] = []
        for peripheral in self.peripherals:
            by_offset: dict[int, list[RegisterDesc]] = {}
            for register in peripheral.registers:
                by_offset.setdefault(register.offset, []).append(register)
            for registers in by_offset.values():
                unlinked = [r for r in registers if r.alternate_of is None]
                if len(unlinked) < 2:
                    continue
                for other in unlinked[1:]:
                    result.append((peripheral.name, unlinked[0].name, other.name))
        return result


@dataclass(slots=True, frozen=True, kw_only=True)
class _Props:
    size: int
    access: Access
    reset_value: int
    reset_mask: int


def _text_or(el: Element, tag: str) -> str | None:
    child = el.find(tag)
    if child is None or child.text is None:
        return None
    text = child.text.strip()
    return text or None


def _parse_int(text: str, *, file: str, key: str) -> int:
    stripped = text.strip()
    try:
        if stripped.startswith(("0x", "0X")):
            return int(stripped, 16)
        if stripped.startswith("#"):
            body = stripped[1:].translate(str.maketrans("xX", "00"))
            return int(body, 2)
        return int(stripped, 10)
    except ValueError as exc:
        raise DescriptionError(file, 0, key, "integer") from exc


def _int_or(el: Element, tag: str, *, file: str, key: str) -> int | None:
    text = _text_or(el, tag)
    if text is None:
        return None
    return _parse_int(text, file=file, key=key)


def _parse_bool(text: str | None) -> bool:
    if text is None:
        return False
    return text.strip().lower() in ("true", "1")


def _to_snake(value: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", value.replace("-", "_")).lower()


def _parse_enum[E: StrEnum](enum_cls: type[E], text: str, *, file: str, key: str) -> E:
    try:
        return enum_cls(_to_snake(text))
    except ValueError:
        valid = ", ".join(member.value for member in enum_cls)
        raise DescriptionError(file, 0, key, f"one of {valid}") from None


def _merge_element(source: Element, derived: Element) -> Element:
    merged = copy.deepcopy(source)
    merged.attrib.pop("derivedFrom", None)
    own_tags = {child.tag for child in derived}
    for tag in own_tags:
        for el in merged.findall(tag):
            merged.remove(el)
        for el in derived.findall(tag):
            merged.append(copy.deepcopy(el))
    return merged


def _resolve_peripherals(originals: dict[str, Element], *, file: str) -> dict[str, Element]:
    resolved: dict[str, Element] = {}
    resolving: list[str] = []

    def resolve(name: str) -> Element:
        if name in resolved:
            return resolved[name]
        if name in resolving:
            chain = " -> ".join([*resolving, name])
            raise DescriptionError(file, 0, "peripheral.derivedFrom", chain)
        el = originals.get(name)
        if el is None:
            raise DescriptionError(file, 0, "peripheral.derivedFrom", f"a peripheral named {name}")
        derived_from = el.get("derivedFrom")
        if derived_from is None:
            resolved[name] = el
            return el
        resolving.append(name)
        source = resolve(derived_from)
        resolving.pop()
        merged = _merge_element(source, el)
        resolved[name] = merged
        return merged

    for name in originals:
        resolve(name)
    return resolved


def _resolve_derived_in_container(
    container: Element,
    tags: tuple[str, ...],
    *,
    file: str,
    label: str,
    recurse_into_clusters: bool,
) -> None:
    by_name: dict[str, Element] = {}
    for child in container:
        if child.tag in tags:
            name = _text_or(child, "name")
            if name is not None:
                by_name[name] = child

    resolved: dict[str, Element] = {}
    resolving: list[str] = []

    def resolve(name: str) -> Element:
        if name in resolved:
            return resolved[name]
        if name in resolving:
            chain = " -> ".join([*resolving, name])
            raise DescriptionError(file, 0, f"{label}.derivedFrom", chain)
        el = by_name.get(name)
        if el is None:
            raise DescriptionError(
                file, 0, f"{label}.derivedFrom", f"a {label} named {name} in the same container"
            )
        derived_from = el.get("derivedFrom")
        if derived_from is None:
            resolved[name] = el
            return el
        if "." in derived_from:
            raise DescriptionError(file, 0, f"{label}.derivedFrom", "a name in the same container")
        resolving.append(name)
        source = resolve(derived_from)
        resolving.pop()
        merged = _merge_element(source, el)
        resolved[name] = merged
        return merged

    replacements = [(el, resolve(name)) for name, el in by_name.items()]
    for old, new in replacements:
        if old is not new:
            index = list(container).index(old)
            container.remove(old)
            container.insert(index, new)

    if recurse_into_clusters:
        for child in container:
            if child.tag == "cluster":
                _resolve_derived_in_container(
                    child, tags, file=file, label=label, recurse_into_clusters=True
                )


def _resolve_fields_derived(container: Element, *, file: str) -> None:
    for child in container:
        if child.tag == "register":
            fields_el = child.find("fields")
            if fields_el is not None:
                _resolve_derived_in_container(
                    fields_el, ("field",), file=file, label="field", recurse_into_clusters=False
                )
        elif child.tag == "cluster":
            _resolve_fields_derived(child, file=file)


def _overlay_props(parent: _Props, el: Element, *, file: str) -> _Props:
    size = _int_or(el, "size", file=file, key="size")
    access_text = _text_or(el, "access")
    reset_value = _int_or(el, "resetValue", file=file, key="resetValue")
    reset_mask = _int_or(el, "resetMask", file=file, key="resetMask")
    return _Props(
        size=size if size is not None else parent.size,
        access=(
            _parse_enum(Access, access_text, file=file, key="access")
            if access_text is not None
            else parent.access
        ),
        reset_value=reset_value if reset_value is not None else parent.reset_value,
        reset_mask=reset_mask if reset_mask is not None else parent.reset_mask,
    )


def _dim_indices(el: Element, *, file: str) -> list[str] | None:
    dim_text = _text_or(el, "dim")
    if dim_text is None:
        return None
    count = _parse_int(dim_text, file=file, key="dim")
    index_text = _text_or(el, "dimIndex")
    if index_text is None:
        return [str(i) for i in range(count)]
    if "," in index_text:
        return [part.strip() for part in index_text.split(",")]
    if "-" in index_text:
        lo_text, hi_text = index_text.split("-", 1)
        lo, hi = _parse_int(lo_text, file=file, key="dimIndex"), _parse_int(
            hi_text, file=file, key="dimIndex"
        )
        return [str(i) for i in range(lo, hi + 1)]
    return [index_text]


def _expand_name(template: str, index: str) -> str:
    if "[%s]" in template:
        return template.replace("[%s]", "") + index
    if "%s" in template:
        return template.replace("%s", index)
    return f"{template}{index}"


def _field_bit_range(field_el: Element, *, file: str) -> tuple[int, int]:
    bit_offset = _int_or(field_el, "bitOffset", file=file, key="field.bitOffset")
    bit_width = _int_or(field_el, "bitWidth", file=file, key="field.bitWidth")
    if bit_offset is not None and bit_width is not None:
        return bit_offset, bit_width
    bit_range = _text_or(field_el, "bitRange")
    if bit_range is not None:
        msb_text, lsb_text = bit_range.strip("[]").split(":", 1)
        msb = _parse_int(msb_text, file=file, key="field.bitRange")
        lsb = _parse_int(lsb_text, file=file, key="field.bitRange")
        return lsb, msb - lsb + 1
    msb = _int_or(field_el, "msb", file=file, key="field.msb")
    lsb = _int_or(field_el, "lsb", file=file, key="field.lsb")
    if msb is not None and lsb is not None:
        return lsb, msb - lsb + 1
    raise DescriptionError(file, 0, "field.bitOffset", "bitOffset/bitWidth, bitRange or msb/lsb")


def _expand_fields(reg_el: Element, register_access: Access, *, file: str) -> list[FieldDesc]:
    fields_el = reg_el.find("fields")
    if fields_el is None:
        return []
    result: list[FieldDesc] = []
    for field_el in fields_el.findall("field"):
        name = _text_or(field_el, "name")
        if name is None:
            raise DescriptionError(file, 0, "field.name", "required")
        lsb, width = _field_bit_range(field_el, file=file)
        access_text = _text_or(field_el, "access")
        acc = (
            _parse_enum(Access, access_text, file=file, key=f"field[{name}].access")
            if access_text is not None
            else register_access
        )
        mwv_text = _text_or(field_el, "modifiedWriteValues")
        on_write = (
            _parse_enum(WriteEffect, mwv_text, file=file, key=f"field[{name}].modifiedWriteValues")
            if mwv_text is not None
            else WriteEffect.MODIFY
        )
        ra_text = _text_or(field_el, "readAction")
        on_read = (
            _parse_enum(ReadEffect, ra_text, file=file, key=f"field[{name}].readAction")
            if ra_text is not None
            else ReadEffect.NONE
        )
        result.append(
            FieldDesc(name=name, lsb=lsb, width=width, acc=acc, on_write=on_write, on_read=on_read)
        )
    return result


def _build_register(
    name: str, offset: int, props: _Props, fields: list[FieldDesc], alternate_of: str | None
) -> RegisterDesc:
    return RegisterDesc(
        name=name,
        offset=offset,
        width=props.size,
        reset=props.reset_value,
        reset_mask=props.reset_mask,
        acc=props.access,
        fields=fields,
        alternate_of=alternate_of,
    )


def _expand_register(
    reg_el: Element, parent_props: _Props, *, prefix: str, base_offset: int, file: str
) -> list[tuple[RegisterDesc, str | None]]:
    props = _overlay_props(parent_props, reg_el, file=file)
    reg_offset = _int_or(reg_el, "addressOffset", file=file, key="register.addressOffset")
    if reg_offset is None:
        raise DescriptionError(file, 0, "register.addressOffset", "required")
    name_template = _text_or(reg_el, "name")
    if name_template is None:
        raise DescriptionError(file, 0, "register.name", "required")
    indices = _dim_indices(reg_el, file=file)
    increment = _int_or(reg_el, "dimIncrement", file=file, key="register.dimIncrement") or 0
    alt_reg = _text_or(reg_el, "alternateRegister")
    alt_group = _text_or(reg_el, "alternateGroup")
    fields = _expand_fields(reg_el, props.access, file=file)
    if indices is None:
        name = f"{prefix}{name_template}"
        return [
            (_build_register(name, base_offset + reg_offset, props, fields, alt_reg), alt_group)
        ]
    results: list[tuple[RegisterDesc, str | None]] = []
    for i, index in enumerate(indices):
        name = f"{prefix}{_expand_name(name_template, index)}"
        offset = base_offset + reg_offset + i * increment
        results.append((_build_register(name, offset, props, fields, alt_reg), alt_group))
    return results


def _expand_cluster(
    cluster_el: Element, parent_props: _Props, *, prefix: str, base_offset: int, file: str
) -> list[tuple[RegisterDesc, str | None]]:
    props = _overlay_props(parent_props, cluster_el, file=file)
    cluster_offset = (
        _int_or(cluster_el, "addressOffset", file=file, key="cluster.addressOffset") or 0
    )
    name_template = _text_or(cluster_el, "name") or ""
    indices = _dim_indices(cluster_el, file=file)
    increment = _int_or(cluster_el, "dimIncrement", file=file, key="cluster.dimIncrement") or 0
    if indices is None:
        return _expand_registers_container(
            cluster_el,
            props,
            prefix=f"{prefix}{name_template}_",
            base_offset=base_offset + cluster_offset,
            file=file,
        )
    results: list[tuple[RegisterDesc, str | None]] = []
    for i, index in enumerate(indices):
        cname = _expand_name(name_template, index)
        offset = base_offset + cluster_offset + i * increment
        results.extend(
            _expand_registers_container(
                cluster_el, props, prefix=f"{prefix}{cname}_", base_offset=offset, file=file
            )
        )
    return results


def _expand_registers_container(
    container: Element, props: _Props, *, prefix: str, base_offset: int, file: str
) -> list[tuple[RegisterDesc, str | None]]:
    results: list[tuple[RegisterDesc, str | None]] = []
    for child in container:
        if child.tag == "register":
            results.extend(
                _expand_register(child, props, prefix=prefix, base_offset=base_offset, file=file)
            )
        elif child.tag == "cluster":
            results.extend(
                _expand_cluster(child, props, prefix=prefix, base_offset=base_offset, file=file)
            )
    return results


def _resolve_alternate_groups(entries: list[tuple[RegisterDesc, str | None]]) -> list[RegisterDesc]:
    anchors: dict[tuple[int, str], str] = {}
    result: list[RegisterDesc] = []
    for register, group in entries:
        if register.alternate_of is None and group:
            key = (register.offset, group)
            anchor = anchors.get(key)
            if anchor is None:
                anchors[key] = register.name
            else:
                register.alternate_of = anchor
        result.append(register)
    return result


def _peripheral_size(periph_el: Element, *, file: str) -> int:
    total = 0
    for block in periph_el.findall("addressBlock"):
        offset = _int_or(block, "offset", file=file, key="addressBlock.offset") or 0
        size = _int_or(block, "size", file=file, key="addressBlock.size") or 0
        total = max(total, offset + size)
    return total


def _peripheral_interrupts(periph_el: Element, *, file: str) -> list[tuple[int, str]]:
    result: list[tuple[int, str]] = []
    for irq_el in periph_el.findall("interrupt"):
        name = _text_or(irq_el, "name")
        value = _int_or(irq_el, "value", file=file, key="interrupt.value")
        if name is None or value is None:
            raise DescriptionError(file, 0, "interrupt", "name and value")
        result.append((value, name))
    return result


def _parse_cpu(device_el: Element, *, file: str) -> SvdCpu:
    cpu_el = device_el.find("cpu")
    if cpu_el is None:
        return SvdCpu()
    return SvdCpu(
        name=_text_or(cpu_el, "name") or "",
        revision=_text_or(cpu_el, "revision") or "",
        fpu_present=_parse_bool(_text_or(cpu_el, "fpuPresent")),
        fpu_dp=_parse_bool(_text_or(cpu_el, "fpuDP")),
        mpu_present=_parse_bool(_text_or(cpu_el, "mpuPresent")),
        dsp_present=_parse_bool(_text_or(cpu_el, "dspPresent")),
        nvic_prio_bits=_int_or(cpu_el, "nvicPrioBits", file=file, key="cpu.nvicPrioBits") or 0,
        vendor_systick_config=_parse_bool(_text_or(cpu_el, "vendorSystickConfig")),
        sau_num_regions=_int_or(cpu_el, "sauNumRegions", file=file, key="cpu.sauNumRegions") or 0,
    )


def expand_svd(data: bytes, *, file: str) -> SvdDevice:
    """SvdDevice of the SVD @p data, DescriptionError when it is malformed, has a derivedFrom
    cycle or names a member with an unknown value."""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        line = exc.position[0] if exc.position else 0
        raise DescriptionError(file, line, "<root>", "valid XML") from exc

    name = _text_or(root, "name") or ""
    version = _text_or(root, "version") or ""
    cpu = _parse_cpu(root, file=file)
    device_props = _overlay_props(
        _Props(size=32, access=Access.READ_WRITE, reset_value=0, reset_mask=0xFFFFFFFF),
        root,
        file=file,
    )

    peripherals_el = root.find("peripherals")
    if peripherals_el is None:
        raise DescriptionError(file, 0, "peripherals", "required")

    originals: dict[str, Element] = {}
    for p in peripherals_el.findall("peripheral"):
        pname = _text_or(p, "name")
        if pname is None:
            raise DescriptionError(file, 0, "peripheral.name", "required")
        originals[pname] = p

    resolved = _resolve_peripherals(originals, file=file)

    peripherals: list[SvdPeripheral] = []
    for pname, original in originals.items():
        merged = resolved[pname]
        base = _int_or(merged, "baseAddress", file=file, key=f"peripheral[{pname}].baseAddress")
        if base is None:
            raise DescriptionError(file, 0, f"peripheral[{pname}].baseAddress", "required")
        size = _peripheral_size(merged, file=file)
        group = _text_or(merged, "groupName") or ""
        derived_from = original.get("derivedFrom")
        props = _overlay_props(device_props, merged, file=file)

        entries: list[tuple[RegisterDesc, str | None]] = []
        registers_el = merged.find("registers")
        if registers_el is not None:
            _resolve_derived_in_container(
                registers_el,
                ("register", "cluster"),
                file=file,
                label="register",
                recurse_into_clusters=True,
            )
            _resolve_fields_derived(registers_el, file=file)
            entries = _expand_registers_container(
                registers_el, props, prefix="", base_offset=0, file=file
            )
        registers = _resolve_alternate_groups(entries)
        interrupts = _peripheral_interrupts(original, file=file)

        peripherals.append(
            SvdPeripheral(
                name=pname,
                base=base,
                size=size,
                group=group,
                derived_from=derived_from,
                registers=registers,
                interrupts=interrupts,
            )
        )

    return SvdDevice(name=name, version=version, cpu=cpu, peripherals=peripherals)
