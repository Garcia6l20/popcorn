from pathlib import Path

from pcons import context

from popcorn_build.config import build_dir, project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest, alias_bare_name

here = Path(__file__).resolve().parent
core = context.current_project.get_target("popcorn_core")

prebuilt_dir = project_dir / "tests" / "firmware" / "image" / "prebuilt"
scratch = build_dir / "test-image-scratch"

firmware_names = ("image", "image-dw4", "image-dw5", "image-dw5gcc", "image-types")
firmware_deps = []
for name in firmware_names:
    _, target = firmware("image", name)
    if target is not None:
        firmware_deps.append(target)

env = {
    "POPCORN_IMAGE_ELF": str(prebuilt_dir / "image.elf"),
    "POPCORN_IMAGE_DW4_ELF": str(prebuilt_dir / "image-dw4.elf"),
    "POPCORN_IMAGE_DW5_ELF": str(prebuilt_dir / "image-dw5.elf"),
    "POPCORN_IMAGE_DW5GCC_ELF": str(prebuilt_dir / "image-dw5gcc.elf"),
    "POPCORN_IMAGE_BIN": str(here / "data" / "image.bin"),
    "POPCORN_IMAGE_HEX": str(here / "data" / "image.hex"),
    "POPCORN_IMAGE_DATA": str(here / "data"),
    "POPCORN_IMAGE_SCRATCH": str(scratch),
}

test_image = add_doctest(
    "image",
    [
        here / "test_arm_attributes.cpp",
        here / "test_image.cpp",
        here / "test_lines.cpp",
        here / "test_load_elf.cpp",
        here / "test_load_raw.cpp",
        here / "test_lookup.cpp",
        here / "test_robustness.cpp",
    ],
    [core],
    env=env,
)
test_image.private.include_dirs.append(str(project_dir / "native" / "src" / "image"))
for target in firmware_deps:
    test_image.depends(target)

image_probe = context.current_project.Program(
    "image-probe", context.current_project.default_environment, sources=[here / "image_probe.cpp"]
)
image_probe.link(core)
alias_bare_name(image_probe)
