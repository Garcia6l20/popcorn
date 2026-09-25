from collections.abc import Mapping
from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import DbIp
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import CoreDesc, IrqDesc, PeripheralDesc
from popcorn.stm32.description._patches import PatchTarget
from popcorn.stm32.description._sources import Sources, find_sources
from popcorn.stm32.description._svd import SvdDevice


@pytest.fixture(scope="session")
def u575_sources() -> Sources:
    """Sources of STM32U575QGIx found on this machine, skips naming what find_sources
    searched."""
    try:
        return find_sources("STM32U575QGIx")
    except DescriptionError as exc:
        pytest.skip(f"STM32U575QGIx sources not found: {exc}")


@pytest.fixture(scope="session")
def st_svd_u575(u575_sources: Sources) -> Path:
    """Path of the local ST STM32U575.svd install, skips naming every path searched."""
    return u575_sources.svd


@pytest.fixture(scope="session")
def st_cubemx_db(u575_sources: Sources) -> Path:
    """Path of the local CubeMX db directory, skips naming the path searched."""
    return u575_sources.mcu_db.parent.parent


@pytest.fixture(scope="session")
def st_cmsis_u575(u575_sources: Sources) -> Path:
    """Path of the local CMSIS stm32u575xx.h header, skips naming the path searched."""
    if u575_sources.cmsis_header is None:
        pytest.skip("stm32u575xx.h not found by find_sources")
    return u575_sources.cmsis_header


def target_from_svd(device: SvdDevice, db_ips: Mapping[str, DbIp] | None = None) -> PatchTarget:
    """PatchTarget built from @p device's peripherals, ip/ip_version from @p db_ips by exact
    instance-name match, falling back to the SVD group or peripheral name."""
    db_ips = db_ips or {}
    peripherals: list[PeripheralDesc] = []
    irqs: dict[int, IrqDesc] = {}
    for sp in device.peripherals:
        matched = db_ips.get(sp.name)
        ip = matched.name if matched is not None else (sp.group or sp.name)
        ip_version = matched.version if matched is not None else ""
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
        for number, name in sp.interrupts:
            irqs.setdefault(number, IrqDesc(number=number, name=name))
    core = CoreDesc(
        cpu=device.cpu.name,
        revision=device.cpu.revision,
        fpu=device.cpu.fpu_present,
        fpu_double=device.cpu.fpu_dp,
        mpu=device.cpu.mpu_present,
        mpu_regions=0,
        mpu_regions_secure=0,
        sau_regions=device.cpu.sau_num_regions,
        trustzone=False,
        dsp=device.cpu.dsp_present,
        nvic_prio_bits=device.cpu.nvic_prio_bits,
        num_irqs=len(irqs),
        vendor_systick_config=device.cpu.vendor_systick_config,
        reset_clock_hz=0,
    )
    return PatchTarget(
        peripherals=list(peripherals), irqs=list(irqs.values()), core=core, db_ips=db_ips
    )


def target_peripheral(target: PatchTarget, name: str) -> PeripheralDesc:
    """The one PeripheralDesc named @p name in @p target, KeyError when there is none."""
    for p in target.peripherals:
        if p.name == name:
            return p
    raise KeyError(name)
