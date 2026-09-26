from pathlib import Path

from pcons import add_subdirectory

here = Path(__file__).resolve().parent

for script in sorted(here.glob("*/pcons-build.py")):
    add_subdirectory(script.parent.name)
