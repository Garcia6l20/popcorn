from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / name for name in ("lines.cpp", "register_file.cpp", "register_layer.cpp", "registry.cpp")
]
sources += [
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
        "tim.cpp",
        "iwdg.cpp",
        "rtc.cpp",
    )
]
