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
        "gpio.cpp",
        "exti.cpp",
        "gpdma.cpp",
        "usart.cpp",
        "uart_wire.cpp",
        "tim.cpp",
        "iwdg.cpp",
        "rtc.cpp",
        "spi.cpp",
        "i2c.cpp",
        "sai.cpp",
        "otg_fs.cpp",
        "adc.cpp",
        "dbgmcu.cpp",
        "crc.cpp",
        "hash.cpp",
    )
]
stm32_entry = here / "stm32" / "plugin.cpp"
usb = [here / "usb" / name for name in ("host.cpp", "cdc_acm.cpp", "hid.cpp", "uac2.cpp")]
devices = [here / "devices" / name for name in ("at25sf.cpp", "i2c_eeprom.cpp", "i2c_table.cpp")]
