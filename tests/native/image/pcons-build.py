from pathlib import Path

from pcons import context

from popcorn_build.config import build_dir, project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
core = context.current_project.get_target("popcorn_core")

_, image_firmware = firmware("image", "image")
image_prebuilt_elf = project_dir / "tests" / "firmware" / "image" / "prebuilt" / "image.elf"
scratch = build_dir / "test-image-scratch"

env = {
    "POPCORN_IMAGE_ELF": str(image_prebuilt_elf),
    "POPCORN_IMAGE_BIN": str(here / "data" / "image.bin"),
    "POPCORN_IMAGE_SCRATCH": str(scratch),
}

test_image = add_doctest(
    "image",
    [
        here / "test_arm_attributes.cpp",
        here / "test_image.cpp",
        here / "test_load_elf.cpp",
        here / "test_lookup.cpp",
        here / "test_robustness.cpp",
    ],
    [core],
    env=env,
)
if image_firmware is not None:
    test_image.depends(image_firmware)
