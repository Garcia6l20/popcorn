from pathlib import Path

here = Path(__file__).resolve().parent
framework = [
    here / name
    for name in ("bus.cpp", "lines.cpp", "register_file.cpp", "register_layer.cpp", "registry.cpp")
]
stm32 = [
    here / "stm32" / name
    for name in (
        "models.cpp",
        "rcc.cpp",
        "caches.cpp",
        "pwr.cpp",
        "flash.cpp",
        "flash_l1.cpp",
        "gpio.cpp",
        "exti.cpp",
        "exti_classic.cpp",
        "gpdma.cpp",
        "dma_classic.cpp",
        "usart.cpp",
        "uart_wire.cpp",
        "tim.cpp",
        "iwdg.cpp",
        "rtc.cpp",
        "rtc_classic.cpp",
        "spi.cpp",
        "spi_medium.cpp",
        "spi_classic.cpp",
        "i2c.cpp",
        "i2c_medium.cpp",
        "i2c_v1.cpp",
        "sai.cpp",
        "mdf.cpp",
        "otg_fs.cpp",
        "usb_drd.cpp",
        "adc.cpp",
        "dbgmcu.cpp",
        "crc.cpp",
        "crs.cpp",
        "hash.cpp",
        "rng.cpp",
        "gtzc.cpp",
        "syscfg.cpp",
        "syscfg_classic.cpp",
        "sbs.cpp",
    )
]
stm32_entry = here / "stm32" / "plugin.cpp"
usb = [here / "usb" / name for name in ("host.cpp", "cdc_acm.cpp", "hid.cpp", "uac2.cpp")]
devices = [here / "devices" / name for name in ("at25sf.cpp", "i2c_eeprom.cpp", "i2c_table.cpp")]
devices_extension = [here / "devices" / "bind.cpp"]
