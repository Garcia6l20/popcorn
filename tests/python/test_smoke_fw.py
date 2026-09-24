from pathlib import Path

from popcorn_testing._smoke import run_to_bkpt

SMOKE_ELF = Path(__file__).parents[1] / "firmware" / "smoke" / "prebuilt" / "smoke.elf"


def fnv1a(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def test_smoke_firmware_reaches_bkpt_with_checksum() -> None:
    assert run_to_bkpt(str(SMOKE_ELF)) == fnv1a(b"popcorn smoke firmware")
