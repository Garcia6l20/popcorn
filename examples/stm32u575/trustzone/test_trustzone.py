"""A secure and Non-secure pair on one STM32U575 with TZEN=1.

The secure part boots from the secure boot address 0x0C000000, links at the 0x0800 alias and
keeps its data in SRAM4 through the 0x28000000 alias, MPCBB4 left Secure. It opens the SAU to
the Non-secure flash, SRAM1 and the peripherals, keeps the last SRAM1 block Secure in MPCBB1,
releases PA5, sets BFHFNMINS and enters the Non-secure reset handler with BLXNS. The
Non-secure ELF embeds the secure binary in `.secure_image` and calls the secure entry
functions through the SG veneers of the CMSE import library. The secure ELF is a symbols-only
image: `image="secure"` resolves its names, its bytes are never loaded.

Boot stops at the Non-secure `main`. Each test writes `ns_case`, an `enum ns_case` the
Non-secure `main` dispatches on, then runs until `ns_done` or the case's own end point.
"""

from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile
from popcorn import Machine, MachineDescription, StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import us

HERE = Path(__file__).parent
SECURE_CODE = range(0x0800_0000, 0x0800_7C00)
NSC = range(0x0800_7C00, 0x0800_8000)
FNC_RETURN = 0xFEFF_FFFF
CASE_BUDGET = 20_000

pytestmark = pytest.mark.popcorn(budget=1_000_000)


def ns(m: Machine, name: str) -> int:
    value = m.var(name, image="nonsecure").value
    assert isinstance(value, int)
    return value


def secure(m: Machine, name: str) -> int:
    value = m.var(name, image="secure").value
    assert isinstance(value, int)
    return value


async def run_case(m: Machine, case: str, until: str = "ns_done", image: str = "nonsecure") -> None:
    m.var("ns_case", image="nonsecure").value = m.image.constants[case]
    stop = await m.run(until=m.sym(until, image=image), budget=CASE_BUDGET)
    assert stop.reason is StopReason.ADDRESS, stop


async def test_boot_reaches_the_nonsecure_main_in_nonsecure_state(trustzone: Machine) -> None:
    assert trustzone.pc == trustzone.sym("main", image="nonsecure")
    assert trustzone.security_state == "nonsecure"


async def test_an_entry_runs_secure_behind_its_veneer_entered_from_nonsecure(
    trustzone: Machine,
) -> None:
    """The veneer's SG executes in Non-secure state, DDI0553B.z B3.20, the function behind it
    in Secure state. Each probe pair is the state's count and its control."""
    function = trustzone.sym("__acle_se_secure_counter_add", image="secure")
    veneer = trustzone.sym("secure_counter_add", image="nonsecure")
    probes = {
        "function secure": trustzone.probe(function, state="secure"),
        "function nonsecure": trustzone.probe(function, state="nonsecure"),
        "veneer nonsecure": trustzone.probe(veneer, state="nonsecure"),
        "veneer secure": trustzone.probe(veneer, state="secure"),
    }
    await run_case(trustzone, "NS_CASE_COUNTER")
    assert ns(trustzone, "ns_result") == 123
    assert {name: probe.count for name, probe in probes.items()} == {
        "function secure": 1,
        "function nonsecure": 0,
        "veneer nonsecure": 1,
        "veneer secure": 0,
    }


async def test_pa5_released_to_nonsecure_is_driven_by_nonsecure_code(trustzone: Machine) -> None:
    """GPIOA_SECCFGR.SEC5 cleared by the secure part, RM0456 13.4.13."""
    pin = trustzone.periph.GPIOA.pin(5)
    assert not pin.level
    await run_case(trustzone, "NS_CASE_COUNTER")
    assert pin.level


async def test_a_nonsecure_read_of_the_secure_sram1_block_reads_zero_and_raises_gtzc(
    trustzone: Machine,
) -> None:
    """RM0456 5.4.2: an MPCBB1 block left Secure reads 0 for a Non-secure access and flags
    TZIC SR4.SRAM1, the secure GTZC_IRQHandler counts it and clears the flag."""
    block = trustzone.sym("__nonsecure_ram_end", image="nonsecure")
    trustzone.mem.u32[block] = 0x5EC2_E7ED
    await run_case(trustzone, "NS_CASE_SECURE_BLOCK")
    assert ns(trustzone, "ns_result") == 0
    assert ns(trustzone, "ns_flag") == 1
    assert secure(trustzone, "secure_illegal_accesses") == 1
    assert trustzone.mem.u32[block] == 0x5EC2_E7ED


async def test_secure_and_nonsecure_systick_tick_at_their_own_rates(trustzone: Machine) -> None:
    """Two SysTick timers, RM0456 3.5.2: 4000 processor clocks per Secure tick, 1000 per
    Non-secure tick, at the 4 MHz MSI after reset, each counted by its own state's handler."""
    await run_case(trustzone, "NS_CASE_TICKS", until="ns_ticking")
    stop = await trustzone.run(for_=us(10_100))
    assert stop.reason is StopReason.TIME, stop
    assert (secure(trustzone, "secure_ticks"), ns(trustzone, "ns_ticks")) == (10, 40)


async def test_the_secure_fp_context_survives_a_nonsecure_callback_that_clobbers_it(
    trustzone: Machine,
) -> None:
    """The secure part is softfp, so `__gnu_cmse_nonsecure_call` saves the context with VLSTM
    and restores it with VLLDM. With FPCCR_S.TS 1, DDI0553B.z C2.4.367 and C2.4.368, the lazy
    preservation the callback's first FP instruction triggers saves S0 to S31, and VLLDM
    restores all 32 after the callback loaded its own value into every one."""
    clobber = trustzone.probe(trustzone.sym("ns_fp_clobber", image="nonsecure"), state="nonsecure")
    await run_case(trustzone, "NS_CASE_FP")
    assert clobber.count == 1
    assert ns(trustzone, "ns_result") == 32


async def test_a_snapshot_inside_a_nonsecure_callback_loads_into_a_fresh_machine(
    trustzone: Machine,
    trustzone_description: MachineDescription,
    machine_factory: MachineFactory,
    tmp_path: Path,
) -> None:
    """Saved in Non-secure state with the Secure caller's FNC_RETURN in lr and its FP context
    lazily pending, the snapshot loaded into a new machine ends as the run without it."""
    await run_case(trustzone, "NS_CASE_FP", until="ns_fp_clobber")
    assert trustzone.security_state == "nonsecure"
    assert trustzone.reg.lr == FNC_RETURN
    trustzone.snapshot().save(tmp_path / "callback.snap")
    stop = await trustzone.run(
        until=trustzone.sym("ns_done", image="nonsecure"), budget=CASE_BUDGET
    )
    assert stop.reason is StopReason.ADDRESS, stop
    ended = (ns(trustzone, "ns_result"), trustzone.instructions, trustzone.time)

    fresh = machine_factory(trustzone_description, booted=False)
    fresh.load_snapshot(tmp_path / "callback.snap")
    assert fresh.security_state == "nonsecure"
    stop = await fresh.run(until=fresh.sym("ns_done", image="nonsecure"), budget=CASE_BUDGET)
    assert stop.reason is StopReason.ADDRESS, stop
    assert (ns(fresh, "ns_result"), fresh.instructions, fresh.time) == ended
    assert ended[0] == 32


async def test_the_symbols_only_secure_image_names_the_embedded_secure_part(
    trustzone: Machine,
) -> None:
    """In a CMSE build the entry name is the SG veneer in both images, `__acle_se_` names the
    function. Memory holds the Non-secure ELF's `.secure_image`, which is the secure ELF's
    loadable content."""
    assert trustzone.sym("__acle_se_secure_counter_add", image="secure") in SECURE_CODE
    veneer = trustzone.sym("secure_counter_add", image="nonsecure")
    assert veneer in NSC
    assert trustzone.sym("secure_counter_add", image="secure") == veneer
    with (HERE / "prebuilt" / "trustzone.elf").open("rb") as file:
        section = ELFFile(file).get_section_by_name(".secure_image")
        assert section is not None
        address, embedded = section["sh_addr"], section.data()
    assert address == SECURE_CODE.start
    assert trustzone.mem.read(address, len(embedded)) == embedded
    with (HERE / "prebuilt" / "trustzone_s.elf").open("rb") as file:
        segments = [
            (segment["p_paddr"], segment.data())
            for segment in ELFFile(file).iter_segments(type="PT_LOAD")
            if segment["p_filesz"]
        ]
    assert segments
    for paddr, data in segments:
        assert trustzone.mem.read(paddr, len(data)) == data
