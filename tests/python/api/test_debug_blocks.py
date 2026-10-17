"""STM32U5 external PPB debug blocks without a model: they read as zero and ignore writes."""

from __future__ import annotations

import asyncio
import logging
import struct
from pathlib import Path

import popcorn
import pytest
from popcorn import OptionBytes, StopReason

ROOT = Path(__file__).parents[3]
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"

CODE = 0x2008_0000
CTI_OFFSET_4 = 0xE004_2004
LOAD_STORE_LOAD = (0x6802, 0x6001, 0x6803, 0xBE01)


@pytest.mark.parametrize("tzen", [False, True])
def test_a_cti_access_from_the_firmware_reads_zero_and_warns_once(
    u575: popcorn.PartDescription, tzen: bool, caplog: pytest.LogCaptureFixture
) -> None:
    """RM0456 75.5: a load, a store and a load of the CTI run on, both loads read 0, and the
    first access logs one model warning naming the block. With TZEN=1 the core runs Secure."""
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), option_bytes=OptionBytes(tzen=tzen))
    m.reset()
    m.mem.write(CODE, struct.pack(f"<{len(LOAD_STORE_LOAD)}H", *LOAD_STORE_LOAD))
    m.reg.r0 = CTI_OFFSET_4
    m.reg.r1 = 0x1234_5678
    m.reg.r2 = 0xDEAD_BEEF
    m.reg.r3 = 0xDEAD_BEEF
    m.reg.pc = CODE
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        stop = asyncio.run(m.run(budget=10))
    assert (stop.reason, m.pc) == (StopReason.USER, CODE + 8)
    assert (m.reg.r2, m.reg.r3) == (0, 0)
    warned = [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "CTI"]
    assert [(vars(r)["popcorn_kind"], vars(r)["popcorn_offset"]) for r in warned] == [
        ("model_warning", 4)
    ]


@pytest.mark.parametrize("tzen", [False, True])
def test_a_cti_access_under_strict_rules_is_a_model_error_and_a_host_read_too(
    u575: popcorn.PartDescription, tzen: bool
) -> None:
    """Under strict_peripherals the firmware's first CTI load raises ModelError naming the
    block and its offset, with no register. A host read of the CTI raises it too."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=OptionBytes(tzen=tzen),
        strict_peripherals=True,
    )
    m.reset()
    m.mem.write(CODE, struct.pack(f"<{len(LOAD_STORE_LOAD)}H", *LOAD_STORE_LOAD))
    m.reg.r0 = CTI_OFFSET_4
    m.reg.pc = CODE
    with pytest.raises(popcorn.ModelError) as caught:
        asyncio.run(m.run(budget=10))
    assert (caught.value.peripheral, caught.value.register, caught.value.offset) == ("CTI", "", 4)
    with pytest.raises(popcorn.ModelError) as host:
        _ = m.mem.u32[CTI_OFFSET_4]
    assert (host.value.peripheral, host.value.register, host.value.offset) == ("CTI", "", 4)


@pytest.mark.parametrize("tzen", [False, True])
def test_a_host_access_to_the_cti_is_a_model_error_and_the_firmware_still_warns_once(
    u575: popcorn.PartDescription, tzen: bool, caplog: pytest.LogCaptureFixture
) -> None:
    """Without strict_peripherals a host read or write of the CTI raises ModelError and logs
    nothing, so the firmware's first access still logs the block's one warning."""
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), option_bytes=OptionBytes(tzen=tzen))
    m.reset()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        with pytest.raises(popcorn.ModelError):
            _ = m.mem.u32[CTI_OFFSET_4]
        with pytest.raises(popcorn.ModelError):
            m.mem.u32[CTI_OFFSET_4] = 1
        with pytest.raises(popcorn.ModelError):
            m.mem.read(CTI_OFFSET_4 - 4, 8)
        m._flush_logs()  # pyright: ignore[reportPrivateUsage]
        assert [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "CTI"] == []
        m.mem.write(CODE, struct.pack(f"<{len(LOAD_STORE_LOAD)}H", *LOAD_STORE_LOAD))
        m.reg.r0 = CTI_OFFSET_4
        m.reg.pc = CODE
        stop = asyncio.run(m.run(budget=10))
    assert stop.reason == StopReason.USER
    warned = [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "CTI"]
    assert len(warned) == 1
