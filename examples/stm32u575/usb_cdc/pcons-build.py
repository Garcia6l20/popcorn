from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "usb_cdc",
    [firmware / "main.c", firmware / "usb_descriptors.c"],
    hal=["pwr_ex"],
    tinyusb=["cdc", "hid"],
)
