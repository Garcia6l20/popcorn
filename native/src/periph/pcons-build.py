from pathlib import Path

here = Path(__file__).resolve().parent
sources = [
    here / name for name in ("lines.cpp", "register_file.cpp", "register_layer.cpp", "registry.cpp")
]
