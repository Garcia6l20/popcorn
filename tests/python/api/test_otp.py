"""The STM32U5 OTP area: read-only to the CPU, erased at power-on, writable by the host."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

import popcorn
import pytest

from .conftest import CALLEE_ELF

OTP = 0x0BFA_0000
OTP_ALIAS = 0x0FFA_0000


def test_otp_reads_erased_and_the_secure_alias_shares_it(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575)
    assert m.mem.read(OTP, 0x200) == b"\xff" * 0x200
    assert m.mem.read(OTP_ALIAS, 16) == b"\xff" * 16
    m.mem.write(OTP + 4, b"\x12\x34")
    assert m.mem.read(OTP, 8) == bytes.fromhex("ffffffff1234ffff")
    assert m.mem.read(OTP_ALIAS, 8) == bytes.fromhex("ffffffff1234ffff")


@pytest.mark.parametrize("address", [OTP, OTP_ALIAS])
def test_a_cpu_store_to_otp_faults(
    u575: popcorn.MachineDescription, m33_json: Path, address: int
) -> None:
    raw = json.loads(m33_json.read_text())
    for region in u575.regions:
        if region.name.startswith("OTP"):
            entry: dict[str, object] = {
                "name": region.name,
                "start": region.start,
                "size": region.size,
                "kind": region.kind.name,
                "acc": {
                    "read": region.acc.read,
                    "write": region.acc.write,
                    "execute": region.acc.execute,
                },
                "secure": region.secure,
            }
            if region.physical is not None:
                entry["physical"] = region.physical
            raw["regions"].append(entry)
    description = popcorn.parse_description(json.dumps(raw), "otp.json")
    m = popcorn.Machine(description, image=popcorn.Image.elf(CALLEE_ELF), arena_base=None)
    m.reset()
    result = asyncio.run(m.call("fill", address, 5, budget=1_000))
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert m.mem.read(OTP, 12) == b"\xff" * 12
