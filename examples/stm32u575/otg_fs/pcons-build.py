from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent

add_example_firmware(
    here, "otg_fs_init", [here / "firmware" / "main.c"], hal=["pcd", "pcd_ex"], ll=["usb"]
)
