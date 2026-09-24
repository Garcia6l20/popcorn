import subprocess
import sys
import tempfile
from pathlib import Path


def loadable_image(objcopy: str, elf: Path, out: Path) -> bytes:
    subprocess.run([objcopy, "-O", "binary", str(elf), str(out)], check=True)
    return out.read_bytes()


def main() -> int:
    objcopy, built, prebuilt, stamp = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    with tempfile.TemporaryDirectory() as scratch:
        if loadable_image(objcopy, built, Path(scratch) / "built") != loadable_image(
            objcopy, prebuilt, Path(scratch) / "prebuilt"
        ):
            print(
                f"{built} differs from {prebuilt} in loadable content, copy it over when intended"
            )
            return 1
    Path(stamp).touch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
