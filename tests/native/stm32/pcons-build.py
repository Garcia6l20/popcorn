from pathlib import Path

from pcons import context

from popcorn_build.config import VARIANT, project_dir
from popcorn_build.firmware import firmware
from popcorn_build.st_description import cached_st_description
from popcorn_build.testing import add_doctest, alias_bare_name, popcorn_libraries

here = Path(__file__).resolve().parent
project = context.current_project
areas = ("stm32", "usb", "armv8m", "core")
shared = popcorn_libraries(*areas)
static = popcorn_libraries(*areas, static=True)
with_devices = popcorn_libraries("devices", *areas)
core_static = popcorn_libraries("core", static=True)[0]
static_with_devices = popcorn_libraries("devices", *areas, static=True)
private_dir = project_dir / "native" / "src" / "periph" / "stm32"
periph_data = project_dir / "tests" / "data" / "periph"

u575 = cached_st_description("STM32U575QGIx", "POPCORN_U575_DESC")
h573 = cached_st_description("STM32H573AIIx", "POPCORN_H573_DESC")
firmwares = {
    name: firmware("periph", name)
    for name in (
        "boot_ll",
        "rcc_ll",
        "pwr_ll",
        "flash_ll",
        "cache_ll",
        "gpio_ll",
        "gpdma_ll",
        "usart_ll",
        "tim_ll",
        "iwdg_ll",
        "rtc_ll",
        "spi_ll",
        "i2c_ll",
        "sai_ll",
        "irq_ll",
        "hash_ll",
        "rng_ll",
    )
}
h573_firmwares = {
    name: firmware("periph", name)
    for name in ("h573_boot_ll", "h573_rcc_ll", "h573_pwr_ll", "h573_flash_ll")
}
h573_env = {
    "POPCORN_H573_DESC": str(h573),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in h573_firmwares.items()},
}
u575_env = {
    "POPCORN_U575_DESC": str(u575),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in firmwares.items()},
    **h573_env,
    "POPCORN_MDF_VECTORS": str(periph_data / "mdf"),
    "POPCORN_FW_USB_CDC": str(
        project_dir / "examples" / "stm32u575" / "usb_cdc" / "prebuilt" / "usb_cdc.elf"
    ),
}

base = add_doctest(
    "stm32-base",
    [here / "test_base.cpp"],
    static,
    env={"POPCORN_STM32_BASE_DESC": str(periph_data / "stm32_base.json")},
)
base.private.include_dirs.append(private_dir)

dfilter_chain = add_doctest(
    "stm32-dfilter-chain",
    [here / "test_dfilter_chain.cpp"],
    static,
    env={"POPCORN_MDF_VECTORS": str(periph_data / "mdf")},
)
dfilter_chain.private.include_dirs.append(private_dir)

add_doctest("stm32-at25sf", [here / "test_at25sf.cpp"], with_devices)
add_doctest("stm32-i2c-devices", [here / "test_i2c_devices.cpp"], with_devices)

u575_programs = [
    add_doctest(
        name,
        [here / source],
        static_with_devices if name in ("stm32-spi", "stm32-i2c") else static,
        env=u575_env,
        register=u575 is not None,
    )
    for name, source in (
        ("stm32-attach-u575", "test_attach_u575.cpp"),
        ("stm32-rcc", "test_rcc.cpp"),
        ("stm32-caches", "test_caches.cpp"),
        ("stm32-power", "test_power.cpp"),
        ("stm32-flash", "test_flash.cpp"),
        ("stm32-flash-secure", "test_flash_secure.cpp"),
        ("stm32-gpio", "test_gpio.cpp"),
        ("stm32-gpdma", "test_gpdma.cpp"),
        ("stm32-lpdma", "test_lpdma.cpp"),
        ("stm32-dma-routing", "test_dma_routing.cpp"),
        ("stm32-usart", "test_usart.cpp"),
        ("stm32-tim", "test_tim.cpp"),
        ("stm32-iwdg", "test_iwdg.cpp"),
        ("stm32-rtc", "test_rtc.cpp"),
        ("stm32-spi", "test_spi.cpp"),
        ("stm32-i2c", "test_i2c.cpp"),
        ("stm32-sai", "test_sai.cpp"),
        ("stm32-mdf", "test_mdf.cpp"),
        ("stm32-adf", "test_adf.cpp"),
        ("stm32-otg-fs", "test_otg_fs.cpp"),
        ("stm32-otg-fs-device", "test_otg_fs_device.cpp"),
        ("stm32-otg-fs-host", "test_otg_fs_host.cpp"),
        ("stm32-adc", "test_adc.cpp"),
        ("stm32-dbgmcu", "test_dbgmcu.cpp"),
        ("stm32-crc", "test_crc.cpp"),
        ("stm32-hash", "test_hash.cpp"),
        ("stm32-rng", "test_rng.cpp"),
        ("stm32-gtzc", "test_gtzc.cpp"),
        ("stm32-tz-aware", "test_tz_aware.cpp"),
        ("stm32-smc", "test_smc.cpp"),
        ("stm32-engine-counters", "test_engine_counters.cpp"),
        ("stm32-h5-names", "test_h5_names.cpp"),
    )
]
for program in u575_programs:
    program.private.include_dirs.append(private_dir)
    for _, target in firmwares.values():
        if target is not None:
            program.depends(target)

attach_h573 = add_doctest(
    "stm32-attach-h573",
    [here / "test_attach_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
attach_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        attach_h573.depends(target)

rcc_h573 = add_doctest(
    "stm32-rcc-h573",
    [here / "test_rcc_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
rcc_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        rcc_h573.depends(target)

power_h573 = add_doctest(
    "stm32-power-h573",
    [here / "test_power_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
power_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        power_h573.depends(target)

flash_h573 = add_doctest(
    "stm32-flash-h573",
    [here / "test_flash_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
flash_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        flash_h573.depends(target)

class_a_h573 = add_doctest(
    "stm32-class-a-h573",
    [here / "test_class_a_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
class_a_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        class_a_h573.depends(target)

gpdma_h573 = add_doctest(
    "stm32-gpdma-h573",
    [here / "test_gpdma_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
gpdma_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        gpdma_h573.depends(target)

adc_h573 = add_doctest(
    "stm32-adc-h573",
    [here / "test_adc_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
adc_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        adc_h573.depends(target)

hash_h573 = add_doctest(
    "stm32-hash-h573",
    [here / "test_hash_h573.cpp"],
    static,
    env=h573_env,
    register=h573 is not None,
)
hash_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        hash_h573.depends(target)

tim_spi_h573 = add_doctest(
    "stm32-tim-spi-h573",
    [here / "test_tim_spi_h573.cpp"],
    static_with_devices,
    env=h573_env,
    register=h573 is not None,
)
tim_spi_h573.private.include_dirs.append(private_dir)
for _, target in h573_firmwares.values():
    if target is not None:
        tim_spi_h573.depends(target)

link_queues = add_doctest("stm32-link-queues", [here / "test_link_queues.cpp"], static)
link_queues.private.include_dirs.append(project_dir / "native" / "src" / "bind")

pty = add_doctest(
    "stm32-pty",
    [here / "test_pty.cpp", project_dir / "native" / "src" / "bind" / "pty.cpp"],
    static,
)
pty.private.include_dirs.append(project_dir / "native" / "src" / "bind")

if u575 is not None:
    bench_irq = project.Program(
        "bench-irq", project.default_environment, sources=[here / "bench_irq.cpp"]
    )
    for library in static:
        bench_irq.link(library)
    bench_irq.private.defines.extend(
        f'{variable}="{value}"' for variable, value in u575_env.items()
    )
    lto = int("-flto=2" in core_static.public.link_flags)
    bench_irq.private.defines.append(f'POPCORN_BENCH_BUILD="variant={VARIANT} lto={lto}"')
    alias_bare_name(bench_irq)
    for _, target in firmwares.values():
        if target is not None:
            bench_irq.depends(target)

uart_wire = add_doctest(
    "stm32-uart-wire",
    [here / "test_uart_wire.cpp"],
    static,
    env=u575_env,
    register=u575 is not None,
)
uart_wire.private.include_dirs.append(private_dir)
for _, target in firmwares.values():
    if target is not None:
        uart_wire.depends(target)

system_uart = add_doctest(
    "stm32-system",
    [here / "test_system_uart.cpp"],
    static,
    env=u575_env,
    register=u575 is not None,
)
system_uart.private.include_dirs.append(private_dir)
for _, target in firmwares.values():
    if target is not None:
        system_uart.depends(target)

if u575 is not None:
    bench_system = project.Program(
        "bench-system", project.default_environment, sources=[here / "bench_system.cpp"]
    )
    for library in shared:
        bench_system.link(library)
    bench_system.private.defines.extend(
        f'{variable}="{value}"' for variable, value in u575_env.items()
    )
    lto = int("-flto=2" in core_static.public.link_flags)
    bench_system.private.defines.append(f'POPCORN_BENCH_BUILD="variant={VARIANT} lto={lto}"')
    alias_bare_name(bench_system)
    for _, target in firmwares.values():
        if target is not None:
            bench_system.depends(target)
