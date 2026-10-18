"""TrustZone Python API: banked registers, the security state and the filtered memory views."""

from __future__ import annotations

import asyncio
import json
import struct
from pathlib import Path

import popcorn
import pytest
from popcorn import OptionBytes
from popcorn.arm import Reg
from popcorn.report import Via, fault_bits, stop_report
from popcorn.units import VirtualTime, ms, us

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


SAU_CTRL = 0xE000_EDD0
SAU_RNR = 0xE000_EDD8
SAU_RBAR = 0xE000_EDDC
SAU_RLAR = 0xE000_EDE0
SRAM_NS = 0x2000_0000
SRAM_S = 0x3000_0000
FLASH_NS = 0x0800_0000
FLASH_S = 0x0C00_0000
PAGE = 0x2000
MPCBB1_SECCFGR0 = 0x5003_2D00
TZSC_SECCFGR2 = 0x5003_2414
TZSC_MPCWM4ACFGR = 0x5003_2470
TZSC_MPCWM4AR = 0x5003_2474
TZIC_SR = tuple(0x5003_2810 + 4 * n for n in range(4))
USART1SEC = 1 << 3
FLASH_SEC1BBR1 = 0x5002_2080
FLASH_SECHDPCR = 0x5002_20C0
GPIOA_ODR = 0x0202_0014
USART1_BRR = 0x0001_380C
BKPSRAM = 0x4003_6400
SECRET = 0x5EC2_E7A5
PUBLIC = 0x0BAD_F00D


def open_machine(u575: popcorn.PartDescription) -> popcorn.Machine:
    """TZEN=1, SECWM1 pages 0 to 7 with HDP1 pages 0 to 3, bank 2 Non-secure. The SAU makes
    the Non-secure flash, SRAM and peripheral aliases Non-secure. MPCBB1 block 1 is
    Non-secure, block 0 stays Secure."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=OptionBytes(tzen=True, secwm1=(0, 7), secwm2=None, hdp1=(0, 3)),
    )
    m.reset()
    for number, (base, limit) in enumerate(
        (
            (FLASH_NS, FLASH_NS + 0x000F_FFE0),
            (SRAM_NS, SRAM_NS + 0x000B_FFE0),
            (0x4000_0000, 0x4FFF_FFE0),
        )
    ):
        m.mem.u32[SAU_RNR] = number
        m.mem.u32[SAU_RBAR] = base
        m.mem.u32[SAU_RLAR] = limit | 1
    m.mem.u32[SAU_CTRL] = 1
    m.mem.u32[MPCBB1_SECCFGR0] &= ~(1 << 1)
    return m


def events(m: popcorn.Machine) -> tuple[int, ...]:
    return tuple(m.mem.u32[at] for at in TZIC_SR)


def test_the_non_secure_view_never_reads_secure_sram(u575: popcorn.PartDescription) -> None:
    """Block 0 of SRAM1 is Secure in MPCBB1: a Non-secure transaction reads 0, the Secure
    alias reads the data, and a Secure request to the Non-secure alias goes out Non-secure,
    DDI0553B.z RXSPQ."""
    m = open_machine(u575)
    m.mem.u32[SRAM_S + 0x100] = SECRET
    m.mem.u32[SRAM_S + 0x200] = PUBLIC
    before = events(m)
    assert m.mem.u32[SRAM_NS + 0x100] == SECRET
    assert m.mem.nonsecure.u32[SRAM_NS + 0x100] == 0
    assert m.mem.nonsecure.read(SRAM_NS + 0xF0, 0x20) == bytes(0x20)
    assert m.mem.secure.u32[SRAM_S + 0x100] == SECRET
    assert m.mem.secure.u32[SRAM_NS + 0x100] == 0
    assert m.mem.nonsecure.u32[SRAM_NS + 0x200] == PUBLIC
    assert m.mem.secure.u32[SRAM_NS + 0x200] == PUBLIC
    assert events(m) == before


def test_a_non_secure_request_to_a_secure_address_is_a_debug_error(
    u575: popcorn.PartDescription,
) -> None:
    """DDI0553B.z B13.3.4 RWMRR: the IDAU makes 0x3xxxxxxx and 0x0Cxxxxxx Secure, the SAU
    leaves 0x9xxxxxxx Secure. A block stops at the first refused access."""
    m = open_machine(u575)
    m.mem.u32[SRAM_S + 0x100] = SECRET
    for at in (SRAM_S + 0x100, FLASH_S + 5 * PAGE, 0x9000_0000):
        with pytest.raises(popcorn.EmulationError, match=f"bus error at {at:#010x}: denied"):
            _ = m.mem.nonsecure.u32[at]
    with pytest.raises(popcorn.EmulationError, match="0x200c0000: denied"):
        m.mem.nonsecure.read(SRAM_NS + 0x000B_FFF0, 0x20)
    with pytest.raises(popcorn.EmulationError, match="denied"):
        m.mem.nonsecure.u32[SRAM_S + 0x100] = 0
    assert m.mem.u32[SRAM_S + 0x100] == SECRET


def test_a_non_secure_view_write_to_a_secure_block_is_dropped(
    u575: popcorn.PartDescription,
) -> None:
    m = open_machine(u575)
    m.mem.u32[SRAM_S + 0x100] = SECRET
    before = events(m)
    m.mem.nonsecure.u32[SRAM_NS + 0x100] = PUBLIC
    m.mem.nonsecure.u32[SRAM_NS + 0x200] = PUBLIC
    assert m.mem.u32[SRAM_S + 0x100] == SECRET
    assert m.mem.u32[SRAM_S + 0x200] == PUBLIC
    assert events(m) == before


def test_the_non_secure_view_never_reads_secure_flash(u575: popcorn.PartDescription) -> None:
    """Page 5 is in SECWM1, page 12 Secure by SEC1BBR1, page 10 Non-secure, RM0456 7.5.2 and
    7.5.4. A write follows the read rule: the Secure pages keep their bytes."""
    m = open_machine(u575)
    m.mem.u32[FLASH_S + 5 * PAGE] = SECRET
    m.mem.u32[FLASH_S + 12 * PAGE] = SECRET
    m.mem.u32[FLASH_S + 10 * PAGE] = PUBLIC
    m.mem.u32[FLASH_SEC1BBR1] = 1 << 12
    before = events(m)
    assert m.mem.nonsecure.u32[FLASH_NS + 5 * PAGE] == 0
    assert m.mem.nonsecure.u32[FLASH_NS + 12 * PAGE] == 0
    assert m.mem.nonsecure.u32[FLASH_NS + 10 * PAGE] == PUBLIC
    assert m.mem.secure.u32[FLASH_S + 5 * PAGE] == SECRET
    assert m.mem.secure.u32[FLASH_S + 12 * PAGE] == SECRET
    m.mem.nonsecure.u32[FLASH_NS + 5 * PAGE] = PUBLIC
    m.mem.nonsecure.u32[FLASH_NS + 12 * PAGE] = PUBLIC
    m.mem.nonsecure.u32[FLASH_NS + 10 * PAGE + 4] = PUBLIC
    assert m.mem.u32[FLASH_S + 5 * PAGE] == SECRET
    assert m.mem.u32[FLASH_S + 12 * PAGE] == SECRET
    assert m.mem.u32[FLASH_S + 10 * PAGE + 4] == PUBLIC
    assert events(m) == before


def test_the_hide_protection_area_reads_zero_to_the_secure_view(
    u575: popcorn.PartDescription,
) -> None:
    """RM0456 7.5.3: with HDP1_ACCDIS set, the HDP area reads 0 to every load and keeps its
    bytes. m.mem still reads it."""
    m = open_machine(u575)
    m.mem.u32[FLASH_S + 2 * PAGE] = SECRET
    assert m.mem.secure.u32[FLASH_S + 2 * PAGE] == SECRET
    m.mem.u32[FLASH_SECHDPCR] = 1
    assert m.mem.secure.u32[FLASH_S + 2 * PAGE] == 0
    assert m.mem.secure.u32[FLASH_S + 5 * PAGE] == m.mem.u32[FLASH_S + 5 * PAGE]
    m.mem.secure.u32[FLASH_S + 2 * PAGE] = PUBLIC
    assert m.mem.u32[FLASH_S + 2 * PAGE] == SECRET


def test_the_non_secure_view_never_reads_a_secure_peripheral_register(
    u575: popcorn.PartDescription,
) -> None:
    """GPIOA pins are Secure at reset, RM0456 Table 128, and USART1 once its TZSC bit is set,
    RM0456 5.4.2: the Non-secure transaction reads 0 and its write is dropped."""
    m = open_machine(u575)
    m.mem.u32[TZSC_SECCFGR2] |= USART1SEC
    m.mem.u32[0x5000_0000 + GPIOA_ODR] = 0xA5A5
    m.mem.u32[0x5000_0000 + USART1_BRR] = 0x1234
    before = events(m)
    assert m.mem.nonsecure.u32[0x4000_0000 + GPIOA_ODR] == 0
    assert m.mem.nonsecure.u32[0x4000_0000 + USART1_BRR] == 0
    assert m.mem.secure.u32[0x4000_0000 + USART1_BRR] == 0
    assert m.mem.secure.u32[0x5000_0000 + GPIOA_ODR] == 0xA5A5
    assert m.mem.secure.u32[0x5000_0000 + USART1_BRR] == 0x1234
    m.mem.nonsecure.u32[0x4000_0000 + USART1_BRR] = 0x99
    assert m.mem.u32[0x5000_0000 + USART1_BRR] == 0x1234
    m.mem.u32[TZSC_SECCFGR2] &= ~USART1SEC
    assert m.mem.nonsecure.u32[0x4000_0000 + USART1_BRR] == 0x1234
    assert events(m) == before


def test_the_non_secure_view_follows_the_backup_sram_watermark(
    u575: popcorn.PartDescription,
) -> None:
    """MPCWM4, RM0456 5.6.8: BKPSRAM is Secure outside its enabled Non-secure sub-region of
    32-byte granules."""
    m = open_machine(u575)
    m.mem.u32[0x5003_6400 + 0x20] = SECRET
    m.mem.u32[0x5003_6400 + 0x60] = SECRET
    assert m.mem.nonsecure.u32[BKPSRAM + 0x20] == 0
    m.mem.u32[TZSC_MPCWM4AR] = 2 << 16 | 1
    m.mem.u32[TZSC_MPCWM4ACFGR] = 1
    assert m.mem.nonsecure.u32[BKPSRAM + 0x20] == SECRET
    assert m.mem.nonsecure.u32[BKPSRAM + 0x60] == 0


def test_the_views_see_their_own_bank_of_the_system_control_space(
    u575: popcorn.PartDescription,
) -> None:
    """The SCS is exempt from attribution: each view reads the bank of its state, and the
    Non-secure view reads the SCS_NS alias as zero, DDI0553B.z RPMNV."""
    m = open_machine(u575)
    m.mem.u32[0xE000_ED08] = 0x0C00_0000
    m.mem.u32[0xE002_ED08] = 0x0801_0000
    assert m.mem.secure.u32[0xE000_ED08] == 0x0C00_0000
    assert m.mem.secure.u32[0xE002_ED08] == 0x0801_0000
    assert m.mem.nonsecure.u32[0xE000_ED08] == 0x0801_0000
    assert m.mem.nonsecure.u32[0xE002_ED08] == 0


def test_the_views_raise_model_error_on_unmodelled_data(u575: popcorn.PartDescription) -> None:
    """The CTI has no model, GPIOA has no register at 0x3FC: ModelError before any access,
    even where the view would refuse it."""
    m = open_machine(u575)
    for view in (m.mem.secure, m.mem.nonsecure):
        for at in (0xE004_2004, 0x4202_03FC, 0x5202_03FC):
            with pytest.raises(popcorn.ModelError):
                _ = view.u32[at]
            with pytest.raises(popcorn.ModelError):
                view.u32[at] = 0


def test_without_trustzone_both_views_act_as_mem(u575: popcorn.PartDescription) -> None:
    m = tz_machine(u575, tzen=False)
    m.mem.u32[SRAM_NS + 0x100] = SECRET
    m.mem.u32[0x4000_0000 + GPIOA_ODR] = 0xA5A5
    for view in (m.mem.secure, m.mem.nonsecure):
        for at in (SRAM_NS + 0x100, SRAM_S + 0x100, 0x4000_0000 + GPIOA_ODR):
            assert view.u32[at] == m.mem.u32[at]
        assert view.read(SRAM_NS, 0x200) == m.mem.read(SRAM_NS, 0x200)
    m.mem.nonsecure.u32[SRAM_S + 0x104] = PUBLIC
    assert m.mem.u32[SRAM_NS + 0x104] == PUBLIC


def test_references_follow_the_view_they_were_built_from(
    u575: popcorn.PartDescription,
) -> None:
    m = open_machine(u575)
    m.mem.u32[SRAM_S + 0x100] = SECRET
    assert asyncio.run(m.read(m.ref.mem.nonsecure.u32[SRAM_NS + 0x100]))[0] == 0
    assert asyncio.run(m.read(m.ref.mem.secure.u32[SRAM_S + 0x100]))[0] == SECRET
    assert asyncio.run(m.read(m.ref.mem.nonsecure.block(SRAM_NS + 0x100, 4)))[0] == bytes(4)
    assert m.ref.mem.nonsecure.u32[SRAM_NS].get() == 0
    with pytest.raises(popcorn.EmulationError, match="denied"):
        asyncio.run(m.read(m.ref.mem.nonsecure.u32[SRAM_S + 0x100]))


def test_no_view_access_raises_an_illegal_access_event(u575: popcorn.PartDescription) -> None:
    """Non-secure writes to the SECCFGR of RCC, PWR, EXTI, SYSCFG and GPDMA1 and Non-secure
    reads of GTZC1_TZSC_CR and FLASH_SECSR are illegal accesses, RM0456 5.4.5: through a view
    they are refused and set no TZIC flag."""
    m = open_machine(u575)
    writes = (0x4602_0D10, 0x4602_0830, 0x4602_2014, 0x4600_0400, 0x4002_0000)
    stored = [m.mem.u32[at + 0x1000_0000] for at in writes]
    before = events(m)
    for at in writes:
        m.mem.nonsecure.u32[at] = 0x5A
    assert m.mem.nonsecure.u32[0x4003_2400] == 0
    _ = m.mem.nonsecure.u32[0x4002_2024]
    assert [m.mem.u32[at + 0x1000_0000] for at in writes] == stored
    assert events(m) == before


SECURE_CALLER = SRAM_S + 0x4000
VENEER = SRAM_S + 0x6000
NONSECURE_CALLER = SRAM_NS + 0x2000
STACK_S = SRAM_S + 0x8000


def veneer_machine(u575: popcorn.PartDescription) -> popcorn.Machine:
    """Secure code calls an SG veneer twice and BLXNS to Non-secure code that calls it three
    times then loops on itself. The veneer is SG then BXNS LR in a Non-secure callable SAU
    region, DDI0553B.z B3.15: run by the Secure caller in Secure state, entered from the
    Non-secure caller in Non-secure state."""
    m = open_machine(u575)
    m.mem.u32[MPCBB1_SECCFGR0] &= ~(1 << ((NONSECURE_CALLER - SRAM_NS) // 0x200))
    m.mem.u32[SAU_RNR] = 3
    m.mem.u32[SAU_RBAR] = VENEER
    m.mem.u32[SAU_RLAR] = VENEER | 3
    m.mem.write(VENEER, struct.pack("<3H", 0xE97F, 0xE97F, 0x4774))
    m.mem.write(SECURE_CALLER, struct.pack("<3H", 0x47A0, 0x47A0, 0x47AC))
    m.mem.write(NONSECURE_CALLER, struct.pack("<4H", 0x47A0, 0x47A0, 0x47A0, 0xE7FE))
    m.reg.msp_s = STACK_S
    m.reg.r4 = VENEER | 1
    m.reg.r5 = NONSECURE_CALLER
    m.reg.pc = SECURE_CALLER
    return m


def veneer_counts(u575: popcorn.PartDescription, running: bool) -> tuple[int, int, int, str | None]:
    m = veneer_machine(u575)
    ns = m.probe(VENEER, state="nonsecure")
    s = m.probe(VENEER, state="secure")
    every = m.probe(VENEER)
    end = NONSECURE_CALLER + 6
    if running:
        loop = m.probe(end)

        async def free() -> bool:
            async with m.running():
                return await loop.hit(within=ms(1))

        assert asyncio.run(free())
    else:
        stop = asyncio.run(m.run(until=end, budget=100))
        assert stop.reason == popcorn.StopReason.ADDRESS
    return ns.count, s.count, every.count, m.security_state


@pytest.fixture(scope="module")
def parked_veneer_counts(u575: popcorn.PartDescription) -> tuple[int, int, int, str | None]:
    return veneer_counts(u575, False)


def test_a_probe_with_a_state_counts_the_arrivals_in_that_state(
    parked_veneer_counts: tuple[int, int, int, str | None],
) -> None:
    assert parked_veneer_counts == (3, 2, 5, "nonsecure")


def test_a_probe_with_a_state_counts_the_same_parked_and_running(
    u575: popcorn.PartDescription,
    parked_veneer_counts: tuple[int, int, int, str | None],
) -> None:
    assert veneer_counts(u575, True) == parked_veneer_counts


def test_a_probe_state_is_checked_before_anything_is_posted(
    u575: popcorn.PartDescription,
) -> None:
    m = open_machine(u575)
    with pytest.raises(ValueError, match="'Secure'"):
        m.probe(VENEER, state="Secure")  # pyright: ignore[reportArgumentType]
    plain = tz_machine(u575, tzen=False)
    with pytest.raises(popcorn.ConfigError, match="TrustZone"):
        plain.probe(VENEER, state="secure")
    assert plain.probe(VENEER).count == 0


NVIC_ITNS0 = 0xE000_E380


def test_an_interrupt_target_is_set_through_the_secure_view_of_itns(
    u575: popcorn.PartDescription,
) -> None:
    """DDI0553B.z D1.2.188: NVIC_ITNSn is RAZ/WI to a Non-secure access."""
    m = open_machine(u575)
    assert not m.nvic.target_nonsecure(5)
    m.nvic.set_target(5, True)
    m.nvic.set_target(37, True)
    assert m.nvic.target_nonsecure(5) and m.nvic.target_nonsecure(37)
    assert not m.nvic.target_nonsecure(4)
    assert m.mem.u32[NVIC_ITNS0] == 1 << 5
    assert m.mem.u32[NVIC_ITNS0 + 4] == 1 << 5
    assert m.mem.nonsecure.u32[NVIC_ITNS0] == 0
    m.nvic.set_target(5, False)
    assert m.mem.u32[NVIC_ITNS0] == 0
    with pytest.raises(ValueError, match="not below"):
        m.nvic.set_target(1000, True)


def test_without_trustzone_every_interrupt_targets_secure(u575: popcorn.PartDescription) -> None:
    m = tz_machine(u575, tzen=False)
    m.nvic.set_target(5, True)
    assert not m.nvic.target_nonsecure(5)


def auviol_stop(u575: popcorn.PartDescription) -> tuple[popcorn.Machine, popcorn.Stop]:
    """Non-secure code loads from a Secure address, DDI0553B.z B10.2: a SecureFault AUVIOL,
    escalated with HFSR.FORCED since SHCSR.SECUREFAULTENA is clear, which stops the run under
    the default fault policy."""
    m = veneer_machine(u575)
    m.mem.write(NONSECURE_CALLER, struct.pack("<2H", 0x6808, 0xE7FE))
    m.reg.msp_ns = SRAM_NS + 0x1000
    m.reg.secure = 0
    m.reg.r1 = SRAM_S + 0x100
    m.reg.pc = NONSECURE_CALLER
    stop = asyncio.run(m.run(budget=10))
    return m, stop


def test_a_secure_fault_report_decodes_sfsr_and_shows_sfar(u575: popcorn.PartDescription) -> None:
    m, stop = auviol_stop(u575)
    assert stop.reason == popcorn.StopReason.FAULT
    assert stop.fault is not None and stop.fault.kind == popcorn.FaultKind.SECURE
    report = stop_report(m, stop, ring=False)
    assert report.fault_bits == {"HFSR": ("FORCED",), "SFSR": ("AUVIOL", "SFARVALID")}
    assert report.sfar == SRAM_S + 0x100
    text = report.text()
    assert "fault: SECURE" in text and "SFSR 0x00000048" in text
    assert "  SFSR: AUVIOL SFARVALID" in text
    assert "  SFAR: 0x30000100" in text


def test_fault_bits_decodes_sfsr_only_when_given() -> None:
    assert fault_bits(1 << 9, 1 << 30) == {"BFSR": ("PRECISERR",), "HFSR": ("FORCED",)}
    assert fault_bits(0, 0, 1 << 0 | 1 << 6) == {"SFSR": ("INVEP", "SFARVALID")}


def test_a_report_without_trustzone_reads_no_sfsr(u575: popcorn.PartDescription) -> None:
    m = tz_machine(u575, tzen=False)
    m.mem.write(SRAM_NS, struct.pack("<2H", 0x6808, 0xE7FE))
    m.reg.r1 = 0x9000_0000
    m.reg.pc = SRAM_NS
    stop = asyncio.run(m.run(budget=10))
    assert stop.fault is not None
    report = stop_report(m, stop, ring=False)
    assert "SFSR" not in report.fault_bits and report.sfar is None


def test_the_call_stack_goes_through_a_blxns_into_the_secure_caller(
    u575: popcorn.PartDescription,
) -> None:
    """DDI0553B.z B3.16: BLXNS leaves FNC_RETURN in lr and pushes the return address on the
    Secure stack. The walk leaves the Non-secure function there for the Secure caller."""
    m = veneer_machine(u575)
    m.mem.write(SECURE_CALLER, struct.pack("<2H", 0x47AC, 0xE7FE))
    m.mem.write(NONSECURE_CALLER, struct.pack("<H", 0xE7FE))
    stop = asyncio.run(m.run(budget=20))
    assert stop.reason == popcorn.StopReason.BUDGET
    assert m.security_state == "nonsecure"
    assert m.reg.lr == 0xFEFF_FFFF
    frames = stop_report(m, stop, ring=False).frames
    assert [(f.pc, f.via) for f in frames] == [
        (NONSECURE_CALLER, Via.PC),
        (SECURE_CALLER + 2, Via.SECURE_CALL),
    ]
    assert frames[1].exact


PAIR = ROOT / "tests" / "firmware" / "api" / "prebuilt"
PAIR_NS = PAIR / "tz_pair.elf"
PAIR_S = PAIR / "tz_pair_s.elf"
SECURE_PART = 0x2020


def patched_text(elf: Path, out: Path) -> int:
    """A copy of `elf` with the first byte of `.text` inverted, from the ELF32 section headers.
    Returns the byte's guest address."""
    data = bytearray(elf.read_bytes())
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    entsize, count, names = struct.unpack_from("<3H", data, 0x2E)
    strtab = struct.unpack_from("<I", data, shoff + names * entsize + 0x10)[0]
    for index in range(count):
        header = shoff + index * entsize
        name, _, _, addr, offset = struct.unpack_from("<5I", data, header)
        if bytes(data[strtab + name : strtab + name + 6]) == b".text\0":
            data[offset] ^= 0xFF
            out.write_bytes(bytes(data))
            return addr
    raise AssertionError(f"{elf} has no .text")


def pair_machine(
    u575: popcorn.PartDescription,
    secure: Path = PAIR_S,
    loaded: bool = False,
    name: str | None = None,
) -> popcorn.Machine:
    """The fixture pair: the Non-secure ELF embeds the secure binary at 0x08000000, the
    secure ELF gives its symbols only. SECWM1 makes pages 0 to 7 Secure."""
    m = popcorn.Machine(
        u575,
        image=[
            popcorn.Image.elf(PAIR_NS, name="nonsecure"),
            popcorn.Image.elf(secure, name="secure", load=loaded),
        ],
        option_bytes=OptionBytes(tzen=True, secwm1=(0, 7), secwm2=None),
        name=name,
    )
    m.reset()
    return m


def test_the_pair_boots_secure_and_calls_back_through_its_veneer(
    u575: popcorn.PartDescription,
) -> None:
    m = pair_machine(u575)
    assert m.security_state == "secure"
    assert m.pc == m.sym("Reset_Handler", image="secure")
    stop = asyncio.run(m.run(until=m.sym("tz_pair_done", image="nonsecure"), budget=10_000))
    assert stop.reason == popcorn.StopReason.ADDRESS
    assert m.security_state == "nonsecure"
    assert m.var("tz_pair_result", image="nonsecure").value == 42
    frames = stop_report(m, stop, ring=False).frames
    assert [(f.location.symbol, f.via) for f in frames] == [
        ("tz_pair_done", Via.PC),
        ("main", Via.PROLOGUE),
        ("__gnu_cmse_nonsecure_call", Via.SECURE_CALL),
        ("main", Via.PROLOGUE),
    ]
    assert frames[3].pc in range(0x0800_0000, 0x0800_0000 + SECURE_PART)
    assert frames[3].location.file.endswith("tz_pair_s.c")


def test_a_symbols_only_secure_image_names_the_embedded_secure_part(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    """The memory holds the Non-secure ELF's `.secure_image` bytes, not the patched secure
    ELF's, while the secure ELF's symbols resolve inside the secure part. In a CMSE build the
    entry name is the SG veneer in both images, `__acle_se_` names the function."""
    patched = tmp_path / "tz_pair_s_patched.elf"
    changed = patched_text(PAIR_S, patched)
    alone = popcorn.Machine(u575, image=popcorn.Image.elf(PAIR_NS))
    m = pair_machine(u575, patched)
    assert m.mem.read(0x0800_0000, SECURE_PART) == alone.mem.read(0x0800_0000, SECURE_PART)
    loaded = pair_machine(u575, patched, loaded=True)
    assert loaded.mem.u8[changed] != m.mem.u8[changed]
    secure_main, nonsecure_main = m.sym("main", image="secure"), m.sym("main", image="nonsecure")
    assert secure_main in range(0x0800_0000, 0x0800_0000 + SECURE_PART)
    assert nonsecure_main >= 0x0801_0000
    assert m.sym("main") == secure_main
    function = m.sym("__acle_se_tz_pair_add", image="secure")
    assert function in range(0x0800_0000, 0x0800_2000)
    assert m.sym("tz_pair_add", image="nonsecure") == m.sym("tz_pair_add", image="secure")
    assert m.sym("tz_pair_add", image="nonsecure") == 0x0800_2000
    assert m.locate(function).symbol == "__acle_se_tz_pair_add"


def test_a_linked_system_keeps_each_member_s_trustzone_its_own(
    u575: popcorn.PartDescription,
) -> None:
    """A TZEN=1 member next to a TZEN=0 one: each reads as alone, and a system snapshot
    restores the security state and the banked registers of the TrustZone member."""
    secure = pair_machine(u575, name="tz")
    plain = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), name="plain")
    plain.reset()
    link = popcorn.UartLink("tz.USART1", "plain.USART1", latency=us(20), baud=460_800)
    with popcorn.System({"tz": secure, "plain": plain}, [link]) as system:
        asyncio.run(system.run(until=VirtualTime(us(100).ps)))
        assert (secure.security_state, plain.security_state) == ("nonsecure", None)
        assert "secure" not in plain.reg.names()
        banked = (secure.reg.msp_s, secure.reg.msp_ns, secure.reg.sp)
        taken = system.snapshot()
        system.reset("tz")
        assert secure.security_state == "secure"
        system.restore(taken)
        assert secure.security_state == "nonsecure"
        assert (secure.reg.msp_s, secure.reg.msp_ns, secure.reg.sp) == banked
        assert plain.security_state is None


def test_coverage_collects_on_a_machine_with_a_symbols_only_image(
    u575: popcorn.PartDescription,
) -> None:
    m = pair_machine(u575)
    m.coverage.enable()
    stop = asyncio.run(m.run(until=m.sym("tz_pair_done", image="nonsecure"), budget=10_000))
    assert stop.reason == popcorn.StopReason.ADDRESS
    coverage = m.coverage.collect()
    assert len(coverage.groups) == 1
    assert [Path(i.path).name for i in coverage.groups[0].images] == ["tz_pair.elf"]
