"""TrustZone Python API: banked registers, the security state and the filtered memory views."""

from __future__ import annotations

import json
from pathlib import Path

import popcorn
import pytest
from popcorn import OptionBytes
from popcorn.arm import Reg

ROOT = Path(__file__).parents[3]
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"

CORE_AND_FP = (
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r5",
    "r6",
    "r7",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "lr",
    "pc",
    "xpsr",
    "primask",
    "basepri",
    "faultmask",
    "control",
    "msp",
    "psp",
    "sp",
    *(f"s{n}" for n in range(32)),
    "fpscr",
)
BANKED = (
    "secure",
    "msp_s",
    "msp_ns",
    "psp_s",
    "psp_ns",
    "control_s",
    "control_ns",
    "primask_s",
    "primask_ns",
    "basepri_s",
    "basepri_ns",
    "faultmask_s",
    "faultmask_ns",
    "msplim_s",
    "msplim_ns",
    "psplim_s",
    "psplim_ns",
)


def tz_machine(u575: popcorn.PartDescription, tzen: bool = True) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), option_bytes=OptionBytes(tzen=tzen))
    m.reset()
    return m


def test_a_tzen_0_machine_lists_the_registers_it_listed_before(
    u575: popcorn.PartDescription,
) -> None:
    m = tz_machine(u575, tzen=False)
    assert m.reg.names() == CORE_AND_FP
    assert m.security_state is None
    with pytest.raises(popcorn.ConfigError, match="no register msp_ns"):
        _ = m.reg.msp_ns


def test_a_tzen_1_machine_appends_the_banked_registers_after_the_others(
    u575: popcorn.PartDescription,
) -> None:
    m = tz_machine(u575)
    assert m.reg.names() == (*CORE_AND_FP, *BANKED)


@pytest.mark.parametrize(("cpu", "trustzone"), [("CM33", False), ("CM4", False), ("CM33", True)])
def test_a_machine_without_trustzone_enabled_has_no_security_state(
    m33_json: Path, cpu: str, trustzone: bool
) -> None:
    """A core without the Security Extension, and a CM33 with it but no option bytes."""
    raw = json.loads(m33_json.read_text())
    raw["core"].update(cpu=cpu, trustzone=trustzone)
    m = popcorn.Machine(popcorn.parse_description(json.dumps(raw), f"{cpu}.json"))
    assert "secure" not in m.reg.names()
    assert m.security_state is None


def test_the_core_resets_secure_and_a_banked_stack_pointer_round_trips(
    u575: popcorn.PartDescription,
) -> None:
    m = tz_machine(u575)
    assert m.reg.secure == 1
    assert m.security_state == "secure"
    assert m.reg.msp_s == m.reg.msp
    m.reg.msp_ns = 0x2000_1000
    m.reg.psp_ns = 0x2000_0800
    assert (m.reg.msp_ns, m.reg.psp_ns) == (0x2000_1000, 0x2000_0800)
    assert m.reg[Reg.MSP_NS] == 0x2000_1000
    assert m.reg.msp == m.reg.msp_s


def test_writing_secure_switches_the_state_and_the_active_stack_pointer(
    u575: popcorn.PartDescription,
) -> None:
    m = tz_machine(u575)
    secure_sp = m.reg.sp
    m.reg.msp_ns = 0x2000_1000
    m.reg.secure = 0
    assert m.security_state == "nonsecure"
    assert m.reg.sp == 0x2000_1000
    assert m.reg.msp_s == secure_sp
    m.reg.secure = 1
    assert m.security_state == "secure"
    assert m.reg.sp == secure_sp


def test_a_reset_returns_to_secure_state(u575: popcorn.PartDescription) -> None:
    m = tz_machine(u575)
    m.reg.secure = 0
    m.reset()
    assert m.security_state == "secure"


def test_a_save_restores_the_state_and_both_banks_on_another_machine(
    u575: popcorn.PartDescription,
) -> None:
    m = tz_machine(u575)
    m.reg.msp_ns = 0x2000_1000
    m.reg.psp_s = 0x2000_0400
    m.reg.secure = 0
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    fresh = tz_machine(u575)
    fresh.restore(saved)
    assert fresh.security_state == "nonsecure"
    assert (fresh.reg.sp, fresh.reg.msp_ns, fresh.reg.psp_s) == (
        0x2000_1000,
        0x2000_1000,
        0x2000_0400,
    )
    assert fresh.reg.msp_s == m.reg.msp_s
