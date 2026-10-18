from pathlib import Path

from popcorn_build.examples import TrustZoneLayout, add_trustzone_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_trustzone_example_firmware(
    here,
    "trustzone",
    [firmware / "secure" / "main.c"],
    [firmware / "nonsecure" / "main.c", firmware / "nonsecure" / "fp_clobber.s"],
    TrustZoneLayout(
        secure_flash=(0x0800_0000, 0x8000),
        nsc_size=0x400,
        secure_ram=(0x2800_0000, 0x4000),
        nonsecure_flash=(0x0800_8000, 0x3_8000),
        nonsecure_ram=(0x2000_0000, 0x2_FE00),
    ),
)
