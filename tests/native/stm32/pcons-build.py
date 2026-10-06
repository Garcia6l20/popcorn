from pathlib import Path

from pcons import context

from popcorn_build.config import VARIANT, project_dir
from popcorn_build.firmware import firmware
from popcorn_build.st_description import cached_st_description
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
private_dir = project_dir / "native" / "src" / "periph" / "stm32"
periph_data = project_dir / "tests" / "data" / "periph"

u575 = cached_st_description("STM32U575QGIx", "POPCORN_U575_DESC")
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
    )
}
u575_env = {
    "POPCORN_U575_DESC": str(u575),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in firmwares.items()},
    "POPCORN_FW_USB_CDC": str(
        project_dir / "examples" / "stm32u575" / "usb_cdc" / "prebuilt" / "usb_cdc.elf"
    ),
}

base = add_doctest(
    "stm32-base",
    [here / "test_base.cpp"],
    [core],
    env={"POPCORN_STM32_BASE_DESC": str(periph_data / "stm32_base.json")},
)
base.private.include_dirs.append(private_dir)

add_doctest("stm32-at25sf", [here / "test_at25sf.cpp"], [core])
add_doctest("stm32-i2c-devices", [here / "test_i2c_devices.cpp"], [core])

u575_programs = [
    add_doctest(name, [here / source], [core], env=u575_env, register=u575 is not None)
    for name, source in (
        ("stm32-attach-u575", "test_attach_u575.cpp"),
        ("stm32-rcc", "test_rcc.cpp"),
        ("stm32-caches", "test_caches.cpp"),
        ("stm32-power", "test_power.cpp"),
        ("stm32-flash", "test_flash.cpp"),
        ("stm32-gpio", "test_gpio.cpp"),
        ("stm32-gpdma", "test_gpdma.cpp"),
        ("stm32-dma-routing", "test_dma_routing.cpp"),
        ("stm32-usart", "test_usart.cpp"),
        ("stm32-tim", "test_tim.cpp"),
        ("stm32-iwdg", "test_iwdg.cpp"),
        ("stm32-rtc", "test_rtc.cpp"),
        ("stm32-spi", "test_spi.cpp"),
        ("stm32-i2c", "test_i2c.cpp"),
        ("stm32-sai", "test_sai.cpp"),
        ("stm32-otg-fs", "test_otg_fs.cpp"),
        ("stm32-otg-fs-device", "test_otg_fs_device.cpp"),
        ("stm32-otg-fs-host", "test_otg_fs_host.cpp"),
        ("stm32-adc", "test_adc.cpp"),
        ("stm32-dbgmcu", "test_dbgmcu.cpp"),
        ("stm32-crc", "test_crc.cpp"),
    )
]
for program in u575_programs:
    program.private.include_dirs.append(private_dir)
    for _, target in firmwares.values():
        if target is not None:
            program.depends(target)

tsan_sources = [project_dir / "tests" / "native" / "tsan_longjmp.cpp"] if VARIANT == "tsan" else []
link_queues = add_doctest(
    "stm32-link-queues", [here / "test_link_queues.cpp", *tsan_sources], [core]
)
link_queues.private.include_dirs.append(project_dir / "native" / "src" / "bind")
