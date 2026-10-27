"""User option bytes: `popcorn.OptionBytes`, `Machine(option_bytes=)` and
`MachineDescription(option_bytes=)`, their checks, the boot address and the FLASH option registers
of the STM32U575, RM0456 7.9.13 to 7.9.22."""

from __future__ import annotations

import logging
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, OptionBytes
from popcorn.board import RccOptions
from popcorn.model import Model

ROOT = Path(__file__).parents[3]
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
FLASH = 0x4002_2000
OPTR = FLASH + 0x40
NSBOOTADD0R = FLASH + 0x44
NSBOOTADD1R = FLASH + 0x48
SECBOOTADD0R = FLASH + 0x4C
SECWM1R1 = FLASH + 0x50
SECWM1R2 = FLASH + 0x54
SECWM2R1 = FLASH + 0x60
SECWM2R2 = FLASH + 0x64
OPTION_REGISTERS = (
    OPTR,
    NSBOOTADD0R,
    NSBOOTADD1R,
    SECBOOTADD0R,
    SECWM1R1,
    SECWM1R2,
    SECWM2R1,
    SECWM2R2,
)
VTOR = 0xE000_ED08
SRAM_S = 0x3000_0000
SRAM_NS = 0x2000_0000
DIGEST_AT_MAIN = "c1fef57e3020d0713b9ead07c1fab2394e8855dce16a87439f02662500bcd068"
DIGEST_WITH_BOOT_AND_RCC_AT_MAIN = (
    "e2cae3f0659a747c33d9f3de170e3145b5fd3a17b90c1a5631db981f4ee3fd85"
)


def booted(
    description: popcorn.PartDescription,
    option_bytes: OptionBytes | None,
    vector_table: int | None = None,
) -> popcorn.Machine:
    m = popcorn.Machine(
        description,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=option_bytes,
        vector_table=vector_table,
    )
    m.reset()
    return m


def readback(m: popcorn.Machine) -> list[int]:
    return [m.mem.u32[at] for at in OPTION_REGISTERS]


def vector_table_at(m: popcorn.Machine, at: int, sp: int) -> int:
    handler = m.mem.u32[0x0800_0004]
    m.mem.u32[at] = sp
    m.mem.u32[at + 4] = handler
    return handler & ~1


def test_the_defaults_are_the_state_after_tzen_goes_from_0_to_1() -> None:
    defaults = OptionBytes()
    assert defaults.tzen is False
    assert defaults.secwm1 == defaults.secwm2 == "all"
    assert defaults.hdp1 is None and defaults.hdp2 is None
    assert defaults.secboot is None and defaults.nsboot is None
    assert defaults.swap_bank is False
    assert defaults.dualbank is None
    assert OptionBytes(secwm1=[1, 2]).secwm1 == (1, 2)  # pyright: ignore[reportArgumentType]


@pytest.mark.parametrize(
    ("changes", "field"),
    [
        ({"tzen": 1}, "tzen"),
        ({"swap_bank": "yes"}, "swap_bank"),
        ({"dualbank": 0}, "dualbank"),
        ({"secwm1": "none"}, "secwm1"),
        ({"secwm1": (3, 2)}, "secwm1"),
        ({"secwm1": (-1, 2)}, "secwm1"),
        ({"secwm2": (1,)}, "secwm2"),
        ({"secwm1": (0, 0x1_0000_0000)}, "secwm1"),
        ({"hdp1": (0, 0x1_0000_0000)}, "hdp1"),
        ({"secwm2": (True, 2)}, "secwm2"),
        ({"hdp1": (0, 1), "secwm1": None}, "hdp1"),
        ({"hdp1": (5, 4)}, "hdp1"),
        ({"hdp2": "all"}, "hdp2"),
        ({"hdp2": (0, 1), "secwm2": None}, "hdp2"),
        ({"secboot": -1}, "secboot"),
        ({"secboot": 0x1_0000_0000}, "secboot"),
        ({"nsboot": True}, "nsboot"),
        ({"nsboot": "0x08000000"}, "nsboot"),
    ],
)
def test_a_value_no_family_accepts_is_refused_at_construction(
    changes: dict[str, object], field: str
) -> None:
    with pytest.raises(ValueError, match=rf"^option_bytes\.{field}: expected "):
        OptionBytes(**changes)  # pyright: ignore[reportArgumentType]


@pytest.mark.parametrize(
    ("option_bytes", "key"),
    [
        (OptionBytes(secwm1=(0, 0x80)), "option_bytes.secwm1"),
        (OptionBytes(secwm1=(0, 64)), "option_bytes.secwm1"),
        (OptionBytes(secwm2=(64, 64)), "option_bytes.secwm2"),
        (OptionBytes(secwm1=(2, 9), hdp1=(3, 5)), "option_bytes.hdp1"),
        (OptionBytes(secwm2=(2, 9), hdp2=(2, 10)), "option_bytes.hdp2"),
        (OptionBytes(hdp1=(1, 4)), "option_bytes.hdp1"),
        (OptionBytes(tzen=True, secboot=0x0C00_0040), "option_bytes.secboot"),
        (OptionBytes(nsboot=0x0800_0001), "option_bytes.nsboot"),
        (OptionBytes(dualbank=False), "option_bytes.dualbank"),
    ],
)
def test_a_value_the_u5_family_refuses_raises_before_the_machine_is_built(
    u575: popcorn.PartDescription, option_bytes: OptionBytes, key: str
) -> None:
    """RM0456 7.9.17 to 7.9.22 page fields of the 64 pages of a U575QG bank, Table 60 HDP
    area inside its secure area from its first page, 7.9.16 128-byte boot address."""
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(u575, option_bytes=option_bytes)
    assert caught.value.key == key
    description = popcorn.MachineDescription(
        part="STM32U575QGIx", images=(), option_bytes=option_bytes
    )
    with pytest.raises(ConfigError) as caught:
        description.machine()
    assert caught.value.key == key
    assert popcorn.MachineDescription(
        part="STM32U575QGIx", images=(), option_bytes=OptionBytes()
    ).machine()


def test_tzen_on_a_core_without_a_security_extension_is_refused(
    m33: popcorn.PartDescription, u575_json: Path
) -> None:
    assert not m33.core.trustzone
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(m33, option_bytes=OptionBytes(tzen=True))
    assert caught.value.key == "option_bytes.tzen"
    no_trustzone = popcorn.load_description(u575_json)
    no_trustzone.core.trustzone = False
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(no_trustzone, option_bytes=OptionBytes(tzen=True))
    assert caught.value.key == "option_bytes.tzen"
    assert popcorn.Machine(no_trustzone, option_bytes=OptionBytes())


def test_tzen_on_a_family_without_an_idau_table_is_refused(m33: popcorn.PartDescription) -> None:
    m33.core.trustzone = True
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(m33, option_bytes=OptionBytes(tzen=True))
    assert caught.value.key == "option_bytes.tzen"
    assert "IDAU" in caught.value.expected


def test_a_part_without_a_flash_model_that_encodes_option_bytes_is_refused(
    m33: popcorn.PartDescription,
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(m33, option_bytes=OptionBytes())
    assert caught.value.key == "option_bytes"
    assert popcorn.Machine(m33)


def test_option_bytes_need_the_built_in_flash_model(u575: popcorn.PartDescription) -> None:
    class Plain(Model):
        pass

    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(u575, models={"FLASH": Plain}, option_bytes=OptionBytes())
    assert caught.value.key == "option_bytes"
    assert "Python" in caught.value.expected
    assert popcorn.Machine(u575, models={"FLASH": Plain})


@pytest.fixture(scope="module")
def u575_json(u575: popcorn.PartDescription) -> Path:
    del u575
    from popcorn.stm32.description import cached_description

    return cached_description("STM32U575QGIx")


def test_the_digest_without_option_bytes_is_the_digest_at_main() -> None:
    plain = popcorn.MachineDescription(part="STM32U575QGIx", images=())
    assert plain.digest == DIGEST_AT_MAIN
    assert (
        plain.replace(boot=popcorn.Boot(vtor=0x0800_0000), rcc=RccOptions()).digest
        == DIGEST_WITH_BOOT_AND_RCC_AT_MAIN
    )
    defaults = plain.replace(option_bytes=OptionBytes())
    secure = plain.replace(option_bytes=OptionBytes(tzen=True))
    assert len({plain.digest, defaults.digest, secure.digest}) == 3
    assert secure.digest == plain.replace(option_bytes=OptionBytes(tzen=True)).digest


def test_a_machine_refuses_option_bytes_of_another_type(u575: popcorn.PartDescription) -> None:
    with pytest.raises(TypeError, match=r"^option_bytes: expected an OptionBytes"):
        popcorn.Machine(u575, option_bytes={"tzen": True})  # pyright: ignore[reportArgumentType]


def test_a_description_refuses_option_bytes_of_another_type() -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.MachineDescription(
            part="STM32U575QGIx",
            images=(),
            option_bytes={"tzen": True},  # pyright: ignore[reportArgumentType]
        )
    assert caught.value.key == "option_bytes"


def test_tzen_boots_from_the_secure_alias_of_the_same_image(
    u575: popcorn.PartDescription,
) -> None:
    """RM0456 4 Table 26 and 7.9.16: TZEN = 1 boots from SECBOOTADD0, default 0x0C000000, the
    secure alias of the flash the image is linked at."""
    plain = booted(u575, None)
    secure = booted(u575, OptionBytes(tzen=True))
    assert secure.reg.pc == secure.mem.u32[0x0C00_0004] & ~1 == plain.reg.pc
    assert secure.reg.sp == plain.reg.sp
    assert secure.mem.u32[VTOR] == 0x0C00_0000
    assert plain.mem.u32[VTOR] == 0x0800_0000


def test_tzen_boots_from_a_secure_boot_address_the_description_sets(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=OptionBytes(tzen=True, secboot=SRAM_S),
    )
    handler = vector_table_at(m, SRAM_S, SRAM_S + 0x1000)
    m.reset()
    assert m.reg.pc == handler
    assert m.reg.sp == SRAM_S + 0x1000
    assert m.mem.u32[VTOR] == SRAM_S


def test_tzen_0_boots_from_the_non_secure_boot_address(u575: popcorn.PartDescription) -> None:
    """RM0456 4 Table 25: TZEN = 0 boots from NSBOOTADD0."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=OptionBytes(secboot=SRAM_S, nsboot=SRAM_NS),
    )
    handler = vector_table_at(m, SRAM_NS, SRAM_NS + 0x2000)
    m.reset()
    assert m.reg.pc == handler
    assert m.reg.sp == SRAM_NS + 0x2000
    assert booted(u575, OptionBytes()).reg.pc == booted(u575, None).reg.pc


def test_the_vector_table_wins_over_the_boot_addresses(u575: popcorn.PartDescription) -> None:
    m = booted(u575, OptionBytes(tzen=True, secboot=SRAM_S), vector_table=0x0800_0000)
    assert m.mem.u32[VTOR] == 0x0800_0000
    assert m.reg.pc == m.mem.u32[0x0800_0004] & ~1


def test_without_option_bytes_the_registers_read_as_at_main(
    u575: popcorn.PartDescription,
) -> None:
    m = booted(u575, None)
    assert readback(m) == [
        0x0000_0000,
        0x0000_000F,
        0x0000_000F,
        0x0000_0000,
        0x0000_0000,
        0x0000_0000,
        0x0000_0000,
        0x0000_0000,
    ]
    m.mem.u32[OPTR] = 0x1234_5678
    assert m.mem.u32[OPTR] == 0
    m.mem.u32[FLASH + 0x08] = 0x4567_0123
    m.mem.u32[FLASH + 0x08] = 0xCDEF_89AB
    m.mem.u32[FLASH + 0x10] = 0x0819_2A3B
    m.mem.u32[FLASH + 0x10] = 0x4C5D_6E7F
    m.mem.u32[OPTR] = 0x1234_5678
    assert m.mem.u32[OPTR] == 0x1234_5678


def test_tzen_0_reads_the_secure_option_registers_as_zero(
    u575: popcorn.PartDescription,
) -> None:
    """RM0456 7.5.1: with TZEN = 0 every secure register is RAZ/WI, SECBOOTADD0R and SECWMxRy
    among them, 7.9.16 to 7.9.22. FLASH_OPTR and NSBOOTADDxR are non-secure, 7.9.13 to 7.9.15."""
    m = booted(u575, OptionBytes(secwm1=(2, 9), hdp1=(2, 5), secboot=SRAM_S))
    assert readback(m) == [0x1FEF_F8AA, 0x0800_007F, 0x0BF9_007F, 0, 0, 0, 0, 0]


def test_tzen_1_reads_the_encoded_option_bytes(u575: popcorn.PartDescription) -> None:
    """RM0456 7.9.13 TZEN bit 31, 7.9.16 SECBOOTADD0 in bits 31:7, 7.9.17 to 7.9.22 PSTRT and
    PEND, HDPxEN and HDPx_PEND, the reserved bits and field bits above 7 at their production
    values. Table 58: the whole bank is PSTRT 0 and PEND 0x7F."""
    m = booted(u575, OptionBytes(tzen=True))
    assert readback(m) == [
        0x9FEF_F8AA,
        0x0800_007F,
        0x0BF9_007F,
        0x0C00_007C,
        0xFFFF_FF80,
        0x7F80_7F80,
        0xFFFF_FF80,
        0x7F80_7F80,
    ]
    m = booted(
        u575,
        OptionBytes(
            tzen=True, secwm1=(2, 9), secwm2=None, hdp1=(2, 5), secboot=SRAM_S, swap_bank=True
        ),
    )
    assert readback(m) == [
        0x9FFF_F8AA,
        0x0800_007F,
        0x0BF9_007F,
        0x3000_007C,
        0xFF89_FF82,
        0xFF85_7F80,
        0xFF80_FFFF,
        0x7F80_7F80,
    ]


def test_writes_to_the_option_registers_are_ignored_and_a_reset_keeps_them(
    u575: popcorn.PartDescription,
) -> None:
    """RM0456 7.9.14 to 7.9.22: the option registers cannot be written while OPTLOCK is set.
    With option bytes configured they also ignore writes after the OPTKEYR unlock."""
    m = booted(u575, OptionBytes(tzen=True, secwm1=(2, 9)))
    expected = readback(m)
    for at in OPTION_REGISTERS:
        m.mem.u32[at] = 0
        m.mem.u8[at + 1] = 0x55
    assert readback(m) == expected
    m.mem.u32[FLASH + 0x08] = 0x4567_0123
    m.mem.u32[FLASH + 0x08] = 0xCDEF_89AB
    m.mem.u32[FLASH + 0x10] = 0x0819_2A3B
    m.mem.u32[FLASH + 0x10] = 0x4C5D_6E7F
    assert m.periph.FLASH.FLASH_NSCR.OPTLOCK == 0
    for at in OPTION_REGISTERS:
        m.mem.u32[at] = 0
    assert readback(m) == expected
    m.reset()
    assert readback(m) == expected
    assert m.periph.FLASH.FLASH_OPTR.TZEN == 1
    assert m.periph.FLASH.FLASH_SECWM1R1.SECWM1_PEND == 0x80 | 9
    m.periph.FLASH.FLASH_SECWM1R1.poke(0)
    assert m.mem.u32[SECWM1R1] == 0
    m.reset()
    assert readback(m) == expected


def test_wrp_registers_are_programmed_by_optstrt_and_survive_a_reset(
    u575: popcorn.PartDescription,
) -> None:
    """RM0456 7.6.1 and 7.9.19: FLASH_WRP1AR reads 0xFF80FFFF, takes a write once OPTLOCK is
    clear, programs on OPTSTRT and reloads from the programmed value at a power-on reset."""
    wrp1ar = FLASH + 0x58
    m = booted(u575, None)
    assert m.mem.u32[wrp1ar] == 0xFF80_FFFF
    m.mem.u32[FLASH + 0x08] = 0x4567_0123
    m.mem.u32[FLASH + 0x08] = 0xCDEF_89AB
    m.mem.u32[FLASH + 0x10] = 0x0819_2A3B
    m.mem.u32[FLASH + 0x10] = 0x4C5D_6E7F
    m.mem.u32[wrp1ar] = 0x8005_0003
    assert m.mem.u32[wrp1ar] == 0xFF85_FF83
    m.mem.u32[FLASH + 0x28] = 1 << 17
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert m.mem.u32[wrp1ar] == 0xFF80_FFFF
    m.reset()
    assert m.mem.u32[wrp1ar] == 0xFF85_FF83


def optstrt_warns(
    description: popcorn.PartDescription,
    caplog: pytest.LogCaptureFixture,
    register: int,
    write: int,
) -> bool:
    m = popcorn.Machine(
        description,
        option_bytes=OptionBytes(tzen=True, secwm1=(2, 9), hdp1=(2, 5)),
    )
    m.reset()
    m.mem.u32[FLASH + 0x08] = 0x4567_0123
    m.mem.u32[FLASH + 0x08] = 0xCDEF_89AB
    m.mem.u32[FLASH + 0x10] = 0x0819_2A3B
    m.mem.u32[FLASH + 0x10] = 0x4C5D_6E7F
    m.mem.u32[register] = write
    caplog.clear()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m.mem.u32[FLASH + 0x28] = 1 << 17
        m.mem.u32[FLASH + 0x28]
    return any(
        getattr(r, "popcorn_kind", None) == "model_warning" and "OPTSTRT" in r.getMessage()
        for r in caplog.records
    )


def test_optstrt_compares_only_the_bits_the_model_encodes(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    """A SECWM write of the machine's own watermarks, or one that changes only the PCROP1
    bits of SECWM1R2, is no unprogrammed change. A changed watermark or HDP1EN is."""
    m = popcorn.Machine(u575, option_bytes=OptionBytes(tzen=True, secwm1=(2, 9), hdp1=(2, 5)))
    m.reset()
    first, second = m.mem.u32[SECWM1R1], m.mem.u32[SECWM1R2]
    assert not optstrt_warns(u575, caplog, SECWM1R1, first)
    assert not optstrt_warns(u575, caplog, SECWM1R2, second)
    assert not optstrt_warns(u575, caplog, SECWM1R2, second ^ 1)
    assert optstrt_warns(u575, caplog, SECWM1R1, first ^ (1 << 16))
    assert optstrt_warns(u575, caplog, SECWM1R2, second ^ (1 << 31))


def test_optstrt_accepts_the_watermarks_of_a_five_bit_part(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """On the STM32U535 the SVD fields are 8 bits wide and the model encodes 5, so the
    firmware's `end << 16 | start` leaves bits 7:5 clear where the loaded word has them set."""
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        u535 = mcu("STM32U535CBTx")
    except DescriptionError as error:
        pytest.skip(f"no STM32U535 description: {error}")
    assert not optstrt_warns(u535, caplog, SECWM1R1, 9 << 16 | 2)
    assert optstrt_warns(u535, caplog, SECWM1R1, 8 << 16 | 2)
