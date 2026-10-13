"""Memory, register and symbol views."""

from __future__ import annotations

import asyncio
import json
import keyword
import math
from pathlib import Path

import popcorn
import pytest
from popcorn.arm import Reg

from .conftest import M33_JSON, Machines

SRAM = 0x2000_8000


@pytest.mark.parametrize(
    ("accessor", "value", "raw"),
    [
        ("u8", 0xAB, b"\xab"),
        ("u16", 0xBEEF, b"\xef\xbe"),
        ("u32", 5, b"\x05\x00\x00\x00"),
        ("u64", 0x0102_0304_0506_0708, bytes.fromhex("0807060504030201")),
        ("i8", -2, b"\xfe"),
        ("i16", -2, b"\xfe\xff"),
        ("i32", -2, b"\xfe\xff\xff\xff"),
        ("i64", -2, b"\xfe" + b"\xff" * 7),
    ],
)
def test_integer_accessors_round_trip(
    machines: Machines, accessor: str, value: int, raw: bytes
) -> None:
    m = machines()
    view: popcorn.ScalarView[int] = getattr(m.mem, accessor)
    view[SRAM] = value
    assert m.mem.read(SRAM, len(raw)) == raw
    assert view[SRAM] == value


@pytest.mark.parametrize(
    ("accessor", "value", "raw"),
    [("f32", 1.5, b"\x00\x00\xc0\x3f"), ("f64", -0.25, bytes.fromhex("000000000000d0bf"))],
)
def test_float_accessors_round_trip(
    machines: Machines, accessor: str, value: float, raw: bytes
) -> None:
    m = machines()
    view: popcorn.ScalarView[float] = getattr(m.mem, accessor)
    view[SRAM] = value
    assert m.mem.read(SRAM, len(raw)) == raw
    assert view[SRAM] == value


def test_spec_section_4_memory_example(machines: Machines) -> None:
    m = machines()
    m.mem.u32[0x2000_0000] = 5
    assert m.mem.read(0x2000_0000, 4) == b"\x05\x00\x00\x00"
    m.mem.write(SRAM, b"hello\0world")
    assert m.mem.cstring(SRAM) == "hello"
    assert m.mem.cstring(SRAM, limit=3) == "hel"
    end = 0x200C_0000
    m.mem.write(end - 70, b"x" * 69 + b"\0")
    assert m.mem.cstring(end - 70, limit=1000) == "x" * 69
    m.mem.write(end - 3, b"abc")
    with pytest.raises(popcorn.EmulationError) as unterminated:
        m.mem.cstring(end - 3, limit=1000)
    assert unterminated.value.address == end
    assert math.isnan(struct_nan(m))


def struct_nan(m: popcorn.Machine) -> float:
    m.mem.f32[SRAM] = math.nan
    return m.mem.f32[SRAM]


def test_an_unmapped_address_is_an_emulation_error_naming_it(machines: Machines) -> None:
    m = machines()
    with pytest.raises(popcorn.EmulationError, match="0x30000000") as read:
        m.mem.read(0x3000_0000, 4)
    assert read.value.address == 0x3000_0000
    with pytest.raises(popcorn.EmulationError, match="0x30000000") as written:
        m.mem.u32[0x3000_0000] = 1
    assert written.value.address == 0x3000_0000
    with pytest.raises(popcorn.EmulationError) as crossing:
        m.mem.read(0x200B_FFFE, 4)
    assert crossing.value.address == 0x200C_0000
    with pytest.raises(popcorn.ConfigError):
        m.mem.read(0xFFFF_FFFE, 4)
    with pytest.raises(ValueError, match="does not fit"):
        m.mem.u8[SRAM] = 256


def test_registers_read_back_by_attribute_and_by_enum(machines: Machines) -> None:
    m = machines()
    m.reg.r0 = 1
    assert m.reg.r0 == 1
    assert m.reg[Reg.R0] == 1
    m.reg[Reg.R7] = 0xDEAD_BEEF
    assert m.reg.r7 == 0xDEAD_BEEF
    assert m.reg[Reg.PSP] == m.reg.psp
    assert m.reg.pc == m.pc
    with pytest.raises(popcorn.ConfigError):
        m.reg.r0 = 1 << 32


def test_the_generated_enum_is_the_arch_module_register_list(machines: Machines) -> None:
    m = machines()
    assert [r.value for r in Reg] == list(m.reg.names())


def test_symbols_resolve_across_images(machines: Machines, driver: popcorn.Image) -> None:
    m = machines()
    assert m.sym("alu_mark") == driver.lookup("alu_mark").value
    assert m.symbols["alu_mark"] == driver.lookup("alu_mark")
    with pytest.raises(popcorn.SymbolError) as caught:
        m.sym("alu_mrk")
    assert "alu_mark" in caught.value.close_matches


def test_views_refuse_access_from_another_task_during_a_run_and_refs_reach_it(
    machines: Machines,
) -> None:
    m = machines()
    word, r0, rw = m.ref.mem.u32[SRAM], m.ref.reg.r0, m.ref.periph.TESTDEV.RW

    async def scenario() -> None:
        task = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        for read in (lambda: m.mem.u32[SRAM], lambda: m.reg.r0, lambda: m.periph.TESTDEV.RW.value):
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                read()
        with pytest.raises(popcorn.MachineRunning):
            m.mem.u32[SRAM] = 0
        with pytest.raises(popcorn.MachineRunning):
            m.reg.r0 = 0
        with pytest.raises(popcorn.MachineRunning):
            m.periph.TESTDEV.RW.value = 0
        written = await m.write((word, 0x1234), (r0, 7), (rw, 0x55))
        values = await m.read(word, r0, rw)
        assert m.busy and values.time >= written.time
        assert all(isinstance(value, int) for value in values)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_a_stop_when_handler_reads_views(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    seen: list[int] = []

    def predicate(machine: popcorn.Machine) -> bool:
        seen.append(machine.reg.pc)
        seen.append(machine.mem.u32[0x2000_0000])
        return True

    stop = asyncio.run(m.run(stop_when=predicate))
    assert stop.reason is popcorn.StopReason.USER
    assert seen


@pytest.mark.parametrize(
    ("cpu", "fpu", "fpu_double", "trustzone"),
    [
        ("CM33", True, False, True),
        ("CM4", True, False, False),
        ("CM7", True, True, False),
        ("CM3", False, False, False),
    ],
)
def test_every_profile_register_is_in_the_generated_enum(
    m33_json: Path, cpu: str, fpu: bool, fpu_double: bool, trustzone: bool
) -> None:
    raw = json.loads(m33_json.read_text())
    raw["core"].update(cpu=cpu, fpu=fpu, fpu_double=fpu_double, trustzone=trustzone)
    m = popcorn.Machine(popcorn.parse_description(json.dumps(raw), f"{cpu}.json"))
    names = {r.value for r in Reg}
    assert set(m.reg.names()) <= names
    assert all(r.name.isidentifier() and not keyword.iskeyword(r.name) for r in Reg)
    assert all(v.isidentifier() and not keyword.iskeyword(v) for v in names)


SRAM_END = 0x2000_0000 + 786432


def test_cstring_reads_up_to_the_end_of_memory_and_raises_past_it(
    m33: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(m33)
    m.mem.write(SRAM_END - 4, b"abc\0")
    assert m.mem.cstring(SRAM_END - 4) == "abc"
    m.mem.write(SRAM_END - 4, b"abcd")
    with pytest.raises(popcorn.EmulationError, match="bus error"):
        m.mem.cstring(SRAM_END - 4)


def test_a_cstring_without_a_nul_at_the_top_of_memory_raises_instead_of_wrapping(
    tmp_path: Path,
) -> None:
    described = json.loads(M33_JSON.read_text())
    described["regions"].append(
        {
            "name": "TOP",
            "start": 0xFFFF_F000,
            "size": 0x1000,
            "kind": "ram",
            "acc": {"read": True, "write": True, "execute": False},
            "secure": False,
        }
    )
    path = tmp_path / "top.json"
    path.write_text(json.dumps(described))
    m = popcorn.Machine(popcorn.load_description(path))
    m.mem.write(0xFFFF_FFC0, b"x" * 64)
    with pytest.raises(popcorn.ConfigError, match="32-bit address space"):
        m.mem.cstring(0xFFFF_FFC0, limit=1000)
