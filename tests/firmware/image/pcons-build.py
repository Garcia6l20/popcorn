from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent
sources = [here / "main.c", here / "shapes.c"]

add_firmware("image", "image", sources)

_, image_dw4 = add_firmware("image", "image-dw4", sources)
if image_dw4 is not None:
    image_dw4.private.compile_flags.extend(["-gdwarf-4", "-Wa,--gdwarf-4"])

_, image_dw5 = add_firmware("image", "image-dw5", sources)
if image_dw5 is not None:
    image_dw5.private.compile_flags.extend(["-gdwarf-5", "-Wa,--gdwarf-5"])

_, image_dw5gcc = add_firmware("image", "image-dw5gcc", sources)
if image_dw5gcc is not None:
    image_dw5gcc.private.compile_flags.extend(["-gdwarf-5", "-gno-as-loc-support"])

types_sources = [here / "types.c", here / "main_types.c"]
_, image_types = add_firmware("image", "image-types", types_sources)
if image_types is not None:
    image_types.private.compile_flags.extend(["-gdwarf-5", "-Wa,--gdwarf-5"])

lto_sources = [here / "lto_main.c", here / "lto_peer.c", here / "lto_memcpy.c"]
_, image_lto = add_firmware("image", "image-lto", lto_sources)
if image_lto is not None:
    image_lto.private.compile_flags.append("-flto")
    image_lto.private.link_flags.extend(["-flto", "-Os", "-g"])
