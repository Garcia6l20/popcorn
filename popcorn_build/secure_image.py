import subprocess
import sys
import tempfile
from pathlib import Path


def binary(objcopy: str, elf: Path, out: Path, *sections: str) -> bytes:
    selected = [option for section in sections for option in ("-j", section)]
    subprocess.run([objcopy, "-O", "binary", *selected, str(elf), str(out)], check=True)
    return out.read_bytes()


def embed(objcopy: str, elf: Path, out: Path, flags: list[str]) -> int:
    with tempfile.TemporaryDirectory() as scratch:
        image = Path(scratch) / f"{elf.stem}.bin"
        binary(objcopy, elf, image)
        source = Path(scratch) / f"{elf.stem}_image.S"
        source.write_text(f'.section .secure_image, "a", %progbits\n.incbin "{image}"\n')
        subprocess.run(
            ["arm-none-eabi-gcc", *flags, "-c", str(source), "-o", str(out)],
            check=True,
        )
    return 0


def check(objcopy: str, secure: Path, nonsecure: Path, stamp: Path) -> int:
    with tempfile.TemporaryDirectory() as scratch:
        expected = binary(objcopy, secure, Path(scratch) / "secure.bin")
        embedded = binary(objcopy, nonsecure, Path(scratch) / "embedded.bin", ".secure_image")
    if embedded != expected:
        print(f"the .secure_image of {nonsecure} is not the binary of {secure}")
        return 1
    stamp.touch()
    return 0


def main() -> int:
    if sys.argv[1] == "--check":
        return check(sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5]))
    objcopy, elf, out, flags = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4:]
    return embed(objcopy, elf, out, flags)


if __name__ == "__main__":
    sys.exit(main())
