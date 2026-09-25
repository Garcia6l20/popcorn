"""Assembles an McuDescription from Sources: expand the SVD, read the db and rzone, apply every
patch, build the IRQ table, then assemble core, regions and peripherals."""

from popcorn.stm32.description._cubemx import instance_ips, read_part, read_rzone
from popcorn.stm32.description._irqs import build_irq_table, read_cmsis_irqs
from popcorn.stm32.description._model import CoreDesc, IrqDesc, McuDescription, PeripheralDesc
from popcorn.stm32.description._patches import PatchTarget, apply_patches, load_patches
from popcorn.stm32.description._sources import Sources, compute_source_digests, generator_string
from popcorn.stm32.description._svd import expand_svd


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


def build_description(sources: Sources) -> McuDescription:
    """McuDescription assembled from @p sources."""
    svd = expand_svd(sources.svd.read_bytes(), file=str(sources.svd))
    db_part = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
    rzone = read_rzone(sources.rzone.read_bytes(), file=str(sources.rzone))
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
        peripherals=peripherals, irqs=list(raw_irqs.values()), core=core, db_ips=db_ips
    )
    loaded_patches = load_patches(sources.patches, part=sources.part, svd_name=sources.svd.name)
    apply_patches(loaded_patches, target)

    patch_irqs = [loaded.patch.irqs for loaded in loaded_patches if loaded.patch.irqs is not None]
    irq_table = build_irq_table(svd, cmsis, patch_irqs, rzone.processor.num_interrupts)

    by_name = {p.name: p for p in target.peripherals}
    for name, numbers in irq_table.by_peripheral.items():
        peripheral = by_name.get(name)
        if peripheral is not None:
            peripheral.irqs = numbers

    target.core.num_irqs = irq_table.num_irqs

    return McuDescription(
        part=sources.part,
        generator=generator_string(),
        sources=compute_source_digests(sources),
        core=target.core,
        regions=rzone.regions,
        peripherals=target.peripherals,
        irqs=irq_table.irqs,
    )
