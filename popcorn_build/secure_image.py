import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    objcopy, elf, out, flags = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4:]
    with tempfile.TemporaryDirectory() as scratch:
        binary = Path(scratch) / f"{elf.stem}.bin"
        subprocess.run([objcopy, "-O", "binary", str(elf), str(binary)], check=True)
        source = Path(scratch) / f"{elf.stem}_image.S"
        source.write_text(f'.section .secure_image, "a", %progbits\n.incbin "{binary}"\n')
        subprocess.run(
            ["arm-none-eabi-gcc", *flags, "-c", str(source), "-o", str(out)],
            check=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
