import os
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import pytest
from popcorn.stm32.description._cubemx import DbIp
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._layers import factor, line_name
from popcorn.stm32.description._model import CoreDesc, IrqDesc, PeripheralDesc
from popcorn.stm32.description._patches import PatchTarget
from popcorn.stm32.description._sources import Sources, find_sources
from popcorn.stm32.description._svd import SvdDevice

PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"


def require_pin_data() -> Path:
    """The STM32_open_pin_data checkout named by POPCORN_STM32_PIN_DATA, skips when unset."""
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    return Path(value)


@pytest.fixture(scope="session")
def u575_sources() -> Sources:
    """Sources of STM32U575QGIx found on this machine, skips naming what find_sources
    searched."""
    try:
        return find_sources("STM32U575QGIx")
    except DescriptionError as exc:
        pytest.skip(f"STM32U575QGIx sources not found: {exc}")


@pytest.fixture(scope="session")
def h573_sources() -> Sources:
    """Sources of STM32H573AIIx found on this machine, skips naming what find_sources searched
    and when the CMSIS header stm32h573xx.h is not among them."""
    try:
        sources = find_sources("STM32H573AIIx")
    except DescriptionError as exc:
        pytest.skip(f"STM32H573AIIx sources not found: {exc}")
    if sources.cmsis_header is None:
        pytest.skip("stm32h573xx.h not found by find_sources, set POPCORN_CMSIS_DEVICE_DIRS")
    return sources


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


def write_layers(
    directory: Path,
    descriptions: Mapping[str, Mapping[str, Any]],
    references: Mapping[str, str] | None = None,
) -> dict[str, bytes]:
    """Layer files of @p descriptions written into @p directory by `_layers.factor`, returned by
    file name. @p references maps a line to its reference part, by default the first name of
    the line in sorted order."""
    if references is None:
        chosen: dict[str, str] = {}
        for name in sorted(descriptions):
            chosen.setdefault(line_name(name), name)
        references = chosen
    files = factor(descriptions, references)
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (directory / name).write_bytes(data)
    return files
