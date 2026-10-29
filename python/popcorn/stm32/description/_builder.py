"""Assembles an PartDescription from Sources: expand the SVD, read the db and rzone, apply every
patch, strip the peripheral prefix of register names, build the IRQ table, then assemble core,
regions and peripherals, GPIO alternate functions and bonded pins included."""

import logging
import re
from collections.abc import Mapping, Sequence

from popcorn.stm32.description._cubemx import (
    Rzone,
    bonded_pins,
    instance_ips,
    read_gpio_alternates,
    read_part,
    read_rzone,
    read_rzone_facts,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._irqs import build_irq_table, read_cmsis_irqs
from popcorn.stm32.description._model import CoreDesc, IrqDesc, PartDescription, PeripheralDesc
from popcorn.stm32.description._patches import (
    PatchReport,
    PatchTarget,
    apply_alternate_drops,
    apply_patches,
    load_patches,
)
from popcorn.stm32.description._sources import Sources, compute_source_digests, generator_string
from popcorn.stm32.description._svd import expand_svd

_LOG = logging.getLogger("popcorn")
_GPIO_PORT_RE = re.compile(r"GPIO([A-Z])")
_DIGITS = "0123456789"


def resolve_ip(
    name: str,
    group: str,
    matched_ip: str | None,
    matched_version: str | None,
    real_ip_names: set[str],
) -> tuple[str, str]:
    if matched_ip is not None:
        return matched_ip, matched_version or ""
    fallback = group or name
    if fallback in real_ip_names:
        return name, ""
    return fallback, ""


def _name_prefixes(name: str) -> set[str]:
    parts = [name, *name.split("_")]
    return {*parts, *(part.rstrip(_DIGITS) for part in parts)}


def bare_register_names(peripheral: PeripheralDesc, ancestors: Sequence[str]) -> dict[str, str]:
    """Strips the prefix SVD names carry, such as `TIM1_` in `TIM1_CR1`, from the registers of
    @p peripheral in place, keeping the SVD name as an alias. A prefix is the IP name, or the
    instance name, one of its `_` parts, either without trailing digits, of the peripheral, of
    the peripheral it aliases or of one of @p ancestors, the instances it derives from. A
    register keeps its name when the bare one is no identifier or names another register.
    `alternate_of` and cross rules follow. Returns the renames, SVD name to bare name."""
    owners = [peripheral.name, *ancestors, *([peripheral.alias_of] if peripheral.alias_of else [])]
    candidates = {peripheral.ip}.union(*(_name_prefixes(owner) for owner in owners)) - {""}
    prefixes = sorted(candidates, key=lambda prefix: (-len(prefix), prefix))
    taken = {register.name for register in peripheral.registers}
    renames: dict[str, str] = {}
    for register in peripheral.registers:
        for prefix in prefixes:
            bare = register.name.removeprefix(f"{prefix}_")
            if bare != register.name and bare.isidentifier() and bare not in taken:
                renames[register.name] = bare
                taken.add(bare)
                break
    for register in peripheral.registers:
        bare = renames.get(register.name)
        if bare is not None:
            register.aliases = [*(register.aliases or []), register.name]
            register.name = bare
        if register.alternate_of is not None:
            register.alternate_of = renames.get(register.alternate_of, register.alternate_of)
    for rule in peripheral.cross_rules:
        rule.reg = renames.get(rule.reg, rule.reg)
        rule.target_reg = renames.get(rule.target_reg, rule.target_reg)
    return renames


def _ancestors(name: str, derived_from: Mapping[str, str | None]) -> list[str]:
    chain: list[str] = []
    parent = derived_from.get(name)
    while parent is not None and parent not in chain:
        chain.append(parent)
        parent = derived_from.get(parent)
    return chain


def _read_rzone_source(sources: Sources) -> Rzone:
    if sources.rzone is not None:
        return read_rzone(sources.rzone.read_bytes(), file=str(sources.rzone))
    if sources.rzone_facts is not None:
        return read_rzone_facts(sources.rzone_facts.read_bytes(), file=str(sources.rzone_facts))
    raise DescriptionError(sources.part, 0, "rzone", "an rzone file or an rzone facts file")


def build_description(sources: Sources) -> PartDescription:
    """PartDescription assembled from @p sources."""
    description, _ = build_description_with_report(sources)
    return description


def build_description_with_report(sources: Sources) -> tuple[PartDescription, PatchReport]:
    """@ref build_description's PartDescription, paired with the PatchReport of every rule
    apply_patches applied while assembling it."""
    svd = expand_svd(sources.svd.read_bytes(), file=str(sources.svd))
    db_part = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
    rzone = _read_rzone_source(sources)
    cmsis = (
        read_cmsis_irqs(
            sources.cmsis_header.read_text(encoding="utf-8"), file=str(sources.cmsis_header)
        )
        if sources.cmsis_header is not None
        else None
    )

    svd_names = {p.name for p in svd.peripherals}
    db_ips = {ip.instance: ip for ip in db_part.ips}
    ip_map = instance_ips(db_part, svd_names, {})
    real_ip_names = {ip.name for ip in db_ips.values()}

    peripherals: list[PeripheralDesc] = []
    for sp in svd.peripherals:
        matched = ip_map.get(sp.name)
        ip, ip_version = resolve_ip(
            sp.name,
            sp.group,
            matched.name if matched is not None else None,
            matched.version if matched is not None else None,
            real_ip_names,
        )
        peripherals.append(
            PeripheralDesc(
                name=sp.name,
                ip=ip,
                ip_version=ip_version,
                base=sp.base,
                size=sp.size,
                registers=sp.registers,
                cross_rules=[],
                irqs=[number for number, _ in sp.interrupts],
            )
        )

    core = CoreDesc(
        cpu=svd.cpu.name,
        revision=svd.cpu.revision,
        fpu=svd.cpu.fpu_present,
        fpu_double=rzone.processor.fpu == "DP_FPU",
        mpu=svd.cpu.mpu_present,
        mpu_regions=max(rzone.processor.mpu_regions - rzone.processor.sec_mpu_regions, 0),
        mpu_regions_secure=rzone.processor.sec_mpu_regions,
        sau_regions=rzone.processor.sau_regions,
        trustzone=rzone.processor.tz,
        dsp=rzone.processor.dsp,
        nvic_prio_bits=svd.cpu.nvic_prio_bits,
        num_irqs=0,
        vendor_systick_config=svd.cpu.vendor_systick_config,
        reset_clock_hz=0,
    )

    raw_irqs: dict[int, IrqDesc] = {}
    for sp in svd.peripherals:
        for number, name in sp.interrupts:
            raw_irqs.setdefault(number, IrqDesc(number=number, name=name))

    target = PatchTarget(
        peripherals=peripherals,
        irqs=list(raw_irqs.values()),
        core=core,
        db_ips=db_ips,
        regions=rzone.regions,
        db_part=db_part,
    )
    loaded_patches = load_patches(sources.patches, part=sources.part, svd_name=sources.svd.name)
    report = apply_patches(loaded_patches, target)

    derived_from = {sp.name: sp.derived_from for sp in svd.peripherals}
    for peripheral in target.peripherals:
        bare_register_names(peripheral, _ancestors(peripheral.name, derived_from))

    patch_irqs = [loaded.patch.irqs for loaded in loaded_patches if loaded.patch.irqs is not None]
    irq_table = build_irq_table(svd, cmsis, patch_irqs, rzone.processor.num_interrupts)

    by_name = {p.name: p for p in target.peripherals}
    for name, numbers in irq_table.by_peripheral.items():
        peripheral = by_name.get(name)
        if peripheral is not None:
            peripheral.irqs = numbers

    target.core.num_irqs = irq_table.num_irqs

    if sources.gpio_modes is None:
        _LOG.warning(
            "%s: no CubeMX GPIO modes file, GPIO ports carry no alternate functions", sources.part
        )
    else:
        alternates = read_gpio_alternates(
            sources.gpio_modes.read_bytes(), file=str(sources.gpio_modes), pins=db_part.pins
        )
        if not alternates:
            _LOG.warning(
                "%s: the CubeMX GPIO modes file selects no alternate function by AF number, as "
                "AFIO remapping parts do, GPIO ports carry no alternate functions",
                sources.part,
            )
        for peripheral in target.peripherals:
            port = _GPIO_PORT_RE.fullmatch(peripheral.name)
            if alternates and port is not None and peripheral.alias_of is None:
                peripheral.alternates = alternates.get(port.group(1), [])
    report.alternates_dropped = apply_alternate_drops(loaded_patches, target.peripherals)
    if db_part.pins:
        bonded = bonded_pins(db_part)
        for peripheral in target.peripherals:
            port = _GPIO_PORT_RE.fullmatch(peripheral.name)
            if port is not None and peripheral.alias_of is None:
                peripheral.bonded = bonded.get(port.group(1), 0)

    description = PartDescription(
        part=sources.part,
        generator=generator_string(),
        sources=compute_source_digests(sources),
        core=target.core,
        regions=target.regions,
        peripherals=target.peripherals,
        irqs=irq_table.irqs,
    )
    return description, report
