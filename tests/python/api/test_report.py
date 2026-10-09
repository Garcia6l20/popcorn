"""Stop and fault reports: fault bits, call stacks, the ring."""

from __future__ import annotations

import asyncio
import dataclasses
import json
import tempfile
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import _core
from popcorn._unwind import (
    MAX_FRAMES,
    SCAN_BYTES,
    Epilogue,
    Function,
    Functions,
    RawFrame,
    Via,
    epilogue,
    prologue,
    scan,
    source_address,
    unwind,
)
from popcorn.model import on_read
from popcorn.report import Report, fault_bits, stop_report

from .conftest import M33_JSON, ROOT
from .models_example import Timer

FAULT_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "fault.elf"

ARM_CFSR = {
    "IACCVIOL": 0,
    "DACCVIOL": 1,
    "MUNSTKERR": 3,
    "MSTKERR": 4,
    "MLSPERR": 5,
    "MMARVALID": 7,
    "IBUSERR": 8,
    "PRECISERR": 9,
    "IMPRECISERR": 10,
    "UNSTKERR": 11,
    "STKERR": 12,
    "LSPERR": 13,
    "BFARVALID": 15,
    "UNDEFINSTR": 16,
    "INVSTATE": 17,
    "INVPC": 18,
    "NOCP": 19,
    "STKOF": 20,
    "UNALIGNED": 24,
    "DIVBYZERO": 25,
}
ARM_HFSR = {"VECTTBL": 1, "FORCED": 30, "DEBUGEVT": 31}


def nested() -> tuple[popcorn.Machine, popcorn.Stop]:
    image = popcorn.Image.elf(FAULT_ELF)
    m = popcorn.Machine(M33_JSON, image=image, vector_table=image.lookup("vt_nested").value)
    m.reset()
    return m, asyncio.run(m.run(budget=10_000))


def in_handler() -> tuple[popcorn.Machine, popcorn.Stop]:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(FAULT_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    asyncio.run(m.run(budget=100))
    m.nvic.pend(0)
    return m, asyncio.run(m.run(budget=1_000))


def test_fault_bit_names_are_the_arm_names() -> None:
    assert {b.name.upper(): b.value for b in _core.CfsrBit} == ARM_CFSR
    assert {b.name.upper(): b.value for b in _core.HfsrBit} == ARM_HFSR
    assert fault_bits(1 << 1 | 1 << 7 | 1 << 9 | 1 << 16 | 1 << 25, 1 << 30) == {
        "MMFSR": ("DACCVIOL", "MMARVALID"),
        "BFSR": ("PRECISERR",),
        "UFSR": ("UNDEFINSTR", "DIVBYZERO"),
        "HFSR": ("FORCED",),
    }
    assert fault_bits(0, 0) == {}


def test_a_nested_fault_reports_its_callers_exactly() -> None:
    m, stop = nested()
    assert stop.reason is popcorn.StopReason.FAULT
    report = stop_report(m, stop)
    assert report.fault_bits == {"UFSR": ("UNDEFINSTR",), "HFSR": ("FORCED",)}
    names = [f.location.symbol for f in report.frames]
    assert names == ["level3", "level2", "level1", "nested_main"]
    assert all(f.exact for f in report.frames)
    assert [f.via for f in report.frames] == [Via.PC] + [Via.PROLOGUE] * 3
    source = FAULT_ELF.parent.parent / "fault.c"
    lines = source.read_text().splitlines()
    assert "udf #7" in lines[report.frames[0].location.line - 1]
    assert "level3(" in lines[report.frames[1].location.line - 1]
    assert "level2(" in lines[report.frames[2].location.line - 1]
    assert "level1(" in lines[report.frames[3].location.line - 1]
    assert all(Path(f.location.file).name == "fault.c" for f in report.frames)
    text = report.text()
    for expected in ("FAULT", "UFSR: UNDEFINSTR", "HFSR: FORCED", "level2", "trace ring:"):
        assert expected in text


def test_a_fault_in_a_handler_unwinds_through_the_exception_frame() -> None:
    m, stop = in_handler()
    report = stop_report(m, stop, ring=False)
    names = [f.location.symbol for f in report.frames]
    assert names == ["in_handler", "IRQ0_Handler", "worker", "Reset_Handler"]
    assert [f.via for f in report.frames] == [Via.PC, Via.PROLOGUE, Via.EXCEPTION, Via.PROLOGUE]
    assert all(f.exact for f in report.frames)
    assert report.ring == ""


def test_a_ring_that_cannot_be_read_does_not_hide_the_report(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    m, stop = nested()
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path / "missing"))
    report = stop_report(m, stop)
    assert report.ring.startswith("ring unavailable: ")
    assert report.frames[0].location.symbol == "level3"


def test_a_plain_stop_reports_without_fault_bits() -> None:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(FAULT_ELF))
    m.reset()
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    report = stop_report(m, stop)
    assert isinstance(report, Report) and report.fault_bits == {}
    assert report.frames[0].location.symbol == "ready"
    assert "fault:" not in report.text()


class Memory:
    """Little-endian words at word addresses, for the unwinder."""

    def __init__(self, halfwords: dict[int, int] | None = None) -> None:
        self.words: dict[int, int] = {}
        for address, value in (halfwords or {}).items():
            self.put16(address, value)

    def put16(self, address: int, value: int) -> None:
        word = self.words.get(address & ~3, 0)
        shift = 16 * ((address >> 1) & 1)
        self.words[address & ~3] = (word & ~(0xFFFF << shift)) | value << shift

    def put32(self, address: int, value: int) -> None:
        self.words[address] = value

    def __call__(self, address: int) -> int | None:
        return self.words.get(address)


@pytest.mark.parametrize(
    ("code", "size", "lr_offset"),
    [
        ([0xB5F0], 20, 16),
        ([0xE92D, 0x4FF0], 36, 32),
        ([0xF84D, 0xED04], 4, 0),
        ([0xB082], 8, None),
        ([0xF5AD, 0x7D80], 256, None),
        ([0xF2AD, 0x3DE8], 1000, None),
        ([0xED2D, 0x8B04], 16, None),
        ([0xED2D, 0x8A03], 12, None),
        ([0xB580, 0xB082], 16, 12),
        ([0xB580, 0x2000], 8, 4),
    ],
)
def test_prologues_decode(code: list[int], size: int, lr_offset: int | None) -> None:
    memory = Memory({0x1000 + 2 * i: h for i, h in enumerate(code)})
    function = Function("f", 0x1000, 0x1100)
    found = prologue(memory, function, 0x1000 + 2 * len(code))
    assert found is not None
    assert (found.size, found.lr_offset) == (size, lr_offset)
    assert prologue(memory, function, 0x1000) is not None
    assert prologue(Memory(), function, 0x1002) is None


def test_the_scan_keeps_only_return_addresses_inside_functions_after_calls() -> None:
    memory = Memory(
        {
            0x2000: 0xF7FF,
            0x2002: 0xFFFE,
            0x2010: 0x4798,
            0x2020: 0x2000,
            0x2030: 0xF7FF,
            0x2032: 0x0000,
            0x9000: 0xF7FF,
            0x9002: 0xFFFE,
        }
    )
    functions = Functions([("caller", 0x2001, 0x40), ("other", 0x3001, 0x10)])
    sp = 0x2000_0000
    words = [0x2005, 0x2013, 0x2023, 0x2004, 0x9005, 0x3001, 0x2035, 0x2005]
    for index, word in enumerate(words):
        memory.put32(sp + 4 * index, word)
    frames = scan(memory, functions, sp)
    assert [f.pc for f in frames] == [0x2004, 0x2012, 0x2004]
    assert all(f.via is Via.SCAN and not f.via.exact for f in frames)
    assert all(functions.at(f.pc) is not None for f in frames)


def test_unwinding_falls_back_to_the_scan_when_a_return_does_not_follow_a_call() -> None:
    memory = Memory({0x1000: 0xB500, 0x1002: 0xBF00, 0x2000: 0xF7FF, 0x2002: 0xFFFE})
    functions = Functions([("leaf", 0x1001, 0x10), ("caller", 0x2001, 0x10)])
    sp = 0x2000_0000
    memory.put32(sp, 0x2009)
    memory.put32(sp + 4, 0x2005)
    frames = unwind(memory, functions, 0x1002, 0, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [(0x1002, Via.PC), (0x2004, Via.SCAN)]


def test_a_size_0_function_reaches_the_next_one() -> None:
    functions = Functions([("first", 0x1001, 0), ("second", 0x1041, 0x10), ("last", 0x2001, 0)])
    found = functions.at(0x1030)
    assert found is not None and found.name == "first"
    last = functions.at(0x2000)
    assert last is not None and last.name == "last" and functions.at(0x2002) is None
    assert functions.at(0x0FFE) is None


def test_an_exception_frame_on_the_process_stack_is_followed() -> None:
    memory = Memory({0x1000: 0xB500, 0x3000: 0xBF00, 0x2000: 0xF7FF, 0x2002: 0xFFFE})
    functions = Functions([("handler", 0x1001, 0x10), ("thread", 0x3001, 0x10)])
    msp, psp = 0x2000_1000, 0x2000_2000
    memory.put32(msp, 0xFFFF_FFFD)
    for index, word in enumerate([0, 0, 0, 0, 0, 0x2005, 0x3002, 0x0100_0000]):
        memory.put32(psp + 4 * index, word)
    frames = unwind(memory, functions, 0x1002, 0xFFFF_FFFD, msp, psp)
    assert [(f.pc, f.via) for f in frames][:2] == [(0x1002, Via.PC), (0x3002, Via.EXCEPTION)]


class CountingTimer(Timer):
    """A Timer counting reads of CNT."""

    reads = 0

    @on_read("CNT")
    def _count_reads(self) -> int:
        type(self).reads += 1
        return 0


def test_a_report_never_reads_a_peripheral() -> None:
    m = popcorn.Machine(
        popcorn.load_description(ROOT / "tests" / "data" / "api" / "model.json"),
        image=popcorn.Image.elf(ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"),
        models={"TIMER": CountingTimer},
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    stop = asyncio.run(m.run(budget=10))
    m.reg.pc = (m.sym("eeprom_roundtrip") & ~1) + 6
    m.reg.sp = 0x4003_0008
    m.reg.lr = 0x4003_000D
    report = stop_report(m, stop, ring=False)
    assert CountingTimer.reads == 0
    assert [f.location.symbol for f in report.frames] == ["eeprom_roundtrip"]
    assert m.mem.u32[0x4003_000C] == 0 and CountingTimer.reads == 1


@pytest.mark.parametrize(
    ("code", "at", "effect"),
    [
        ([0xBD10], 0, Epilogue(8, 4)),
        ([0xB002, 0xBD10], 0, Epilogue(16, 12)),
        ([0xB002, 0xBD10], 2, Epilogue(8, 4)),
        ([0xF10D, 0x0D10, 0xBD00], 0, Epilogue(20, 16)),
        ([0xF20D, 0x3DE8, 0xBD00], 0, Epilogue(1004, 1000)),
        ([0xECBD, 0x8B04, 0xBD00], 0, Epilogue(20, 16)),
        ([0xBC10, 0x4770], 0, Epilogue(4, None)),
        ([0xE8BD, 0x8FF0], 0, Epilogue(36, 32)),
        ([0xF85D, 0xFB04], 0, Epilogue(4, 0)),
        ([0xE8BD, 0x4010, 0x4770], 0, Epilogue(8, 4)),
        ([0xF85D, 0xEB04, 0x4770], 0, Epilogue(4, 0)),
        ([0xB002, 0xF85D, 0xEB04, 0x4770], 0, Epilogue(12, 8)),
        ([0x4770], 0, Epilogue(0, None)),
    ],
)
def test_epilogues_simulate_to_the_return(code: list[int], at: int, effect: Epilogue) -> None:
    memory = Memory({0x1000 + 2 * i: h for i, h in enumerate(code)})
    assert epilogue(memory, 0x1000 + at) == effect


def test_only_epilogue_forms_start_an_epilogue() -> None:
    assert epilogue(Memory({0x1000: 0x2000}), 0x1000) is False
    assert epilogue(Memory({0x1000: 0xB002, 0x1002: 0x2000}), 0x1000) is None
    assert epilogue(Memory(), 0x1000) is False


def test_a_stop_on_an_epilogue_unwinds_from_it() -> None:
    code = [0xB510, 0xB082, 0xBF00, 0xB002, 0xBD10]
    memory = Memory({0x1000 + 2 * i: h for i, h in enumerate(code)})
    memory.put16(0x2000, 0xF7FF)
    memory.put16(0x2002, 0xFFFE)
    functions = Functions([("callee", 0x1001, 10), ("caller", 0x2001, 0x10)])
    sp = 0x2000_0000
    memory.put32(sp + 4, 0x2005)
    frames = unwind(memory, functions, 0x1008, 0, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [(0x1008, Via.PC), (0x2004, Via.PROLOGUE)]
    frames = unwind(memory, functions, 0x1006, 0, sp - 8, 0)
    assert [(f.pc, f.via) for f in frames] == [(0x1006, Via.PC), (0x2004, Via.PROLOGUE)]


def test_an_epilogue_that_cannot_be_followed_is_scanned() -> None:
    memory = Memory({0x1000: 0xB002, 0x1002: 0x2000, 0x2000: 0xF7FF, 0x2002: 0xFFFE})
    functions = Functions([("callee", 0x1001, 8), ("caller", 0x2001, 0x10)])
    sp = 0x2000_0000
    memory.put32(sp, 0)
    memory.put32(sp + 4, 0x2005)
    frames = unwind(memory, functions, 0x1000, 0, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [(0x1000, Via.PC), (0x2004, Via.SCAN)]


def test_a_call_ending_its_function_names_the_caller() -> None:
    memory = Memory(
        {
            0x1000: 0xB500,
            0x2000: 0xB500,
            0x2002: 0xBF00,
            0x2004: 0xBF00,
            0x2006: 0xBF00,
            0x2008: 0xBF00,
            0x200A: 0xBF00,
            0x200C: 0xF7FF,
            0x200E: 0xFFFE,
            0x4000: 0xF7FF,
            0x4002: 0xFFFE,
        }
    )
    functions = Functions([("callee", 0x1001, 8), ("caller", 0x2001, 0x10), ("outer", 0x4001, 8)])
    sp = 0x2000_0000
    memory.put32(sp, 0x2011)
    memory.put32(sp + 4, 0x4005)
    frames = unwind(memory, functions, 0x1002, 0, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [
        (0x1002, Via.PC),
        (0x2010, Via.PROLOGUE),
        (0x4004, Via.PROLOGUE),
    ]
    assert functions.at(source_address(memory, frames[1])) == functions.at(0x2000)
    assert [f.pc for f in scan(memory, functions, sp)] == [0x2010, 0x4004]


def exception(
    exc_return: int, xpsr: int = 0x0100_0000, *, on_psp: bool = False
) -> tuple[Memory, Functions, int, int]:
    """A handler that pushed {r7, lr} with lr = exc_return, over a thread frame at `base`."""
    memory = Memory({0x1000: 0xB580, 0x3000: 0xB580, 0x4000: 0xF7FF, 0x4002: 0xFFFE})
    functions = Functions(
        [("handler", 0x1001, 0x10), ("thread", 0x3001, 0x10), ("outer", 0x4001, 0x10)]
    )
    msp, psp = 0x2000_1000, 0x2000_3000
    memory.put32(msp + 4, exc_return)
    base = psp if on_psp else msp + 8
    for index, word in enumerate([0, 0, 0, 0, 0, 0, 0x3002, xpsr]):
        memory.put32(base + 4 * index, word)
    return memory, functions, msp, base


@pytest.mark.parametrize(
    ("exc_return", "xpsr", "on_psp", "size"),
    [
        (0xFFFF_FFF9, 0x0100_0000, False, 32),
        (0xFFFF_FFE9, 0x0100_0000, False, 104),
        (0xFFFF_FFED, 0x0100_0000, True, 104),
        (0xFFFF_FFF9, 0x0100_0200, False, 36),
        (0xFFFF_FFFD, 0x0100_0000, True, 32),
    ],
)
def test_the_stack_continues_past_an_exception_frame(
    exc_return: int, xpsr: int, on_psp: bool, size: int
) -> None:
    memory, functions, msp, base = exception(exc_return, xpsr, on_psp=on_psp)
    memory.put32(base + size + 4, 0x4005)
    frames = unwind(memory, functions, 0x1002, 0, msp, 0x2000_3000)
    assert [(f.pc, f.via) for f in frames] == [
        (0x1002, Via.PC),
        (0x3002, Via.EXCEPTION),
        (0x4004, Via.PROLOGUE),
    ]


@pytest.mark.parametrize("exc_return", [0xFFFF_FFBC, 0xFFFF_FFDD, 0xFFFF_FF98])
def test_non_secure_or_additional_state_frames_are_scanned(exc_return: int) -> None:
    memory, functions, msp, _ = exception(exc_return, on_psp=bool(exc_return & 0b100))
    frames = unwind(memory, functions, 0x1002, 0, msp, 0x2000_3000)
    assert all(f.via is not Via.EXCEPTION for f in frames)


def test_a_trustzone_core_unwinds_a_handler_fault_exactly(tmp_path: Path, m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["core"]["trustzone"] = True
    path = tmp_path / "m33_tz.json"
    path.write_text(json.dumps(raw))
    description = popcorn.load_description(path)
    assert description.core.trustzone
    m = popcorn.Machine(description, image=popcorn.Image.elf(FAULT_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    asyncio.run(m.run(budget=100))
    m.nvic.pend(0)
    report = stop_report(m, asyncio.run(m.run(budget=1_000)), ring=False)
    assert [f.via for f in report.frames] == [Via.PC, Via.PROLOGUE, Via.EXCEPTION, Via.PROLOGUE]
    assert all(f.exact for f in report.frames)


def test_an_exception_after_an_svc_names_the_svc() -> None:
    memory = Memory({0x3000: 0xDF05, 0x3002: 0xBF00, 0x3010: 0x2000, 0x3012: 0xBF00})
    assert source_address(memory, RawFrame(0x3002, Via.EXCEPTION)) == 0x3000
    assert source_address(memory, RawFrame(0x3012, Via.EXCEPTION)) == 0x3012
    assert source_address(memory, RawFrame(0x3012, Via.PROLOGUE)) == 0x3010
    assert source_address(memory, RawFrame(0x3012, Via.PC)) == 0x3012


def test_the_scan_reads_1_kib_of_stack() -> None:
    memory = Memory({0x2000: 0xF7FF, 0x2002: 0xFFFE})
    functions = Functions([("caller", 0x2001, 0x10)])
    sp = 0x2000_0000
    for offset in range(0, SCAN_BYTES + 8, 4):
        memory.put32(sp + offset, 0)
    memory.put32(sp + SCAN_BYTES - 4, 0x2005)
    memory.put32(sp + SCAN_BYTES, 0x2005)
    assert [f.pc for f in scan(memory, functions, sp)] == [0x2004]


def test_a_runaway_recursion_stops_at_the_frame_cap() -> None:
    memory = Memory({0x1000: 0xB500, 0x1002: 0xBF00, 0x1004: 0xF7FF, 0x1006: 0xFFFE})
    functions = Functions([("recurse", 0x1001, 0x10)])
    sp = 0x2000_0000
    for index in range(2 * MAX_FRAMES):
        memory.put32(sp + 4 * index, 0x1009)
    frames = unwind(memory, functions, 0x1002, 0, sp, 0)
    assert len(frames) == MAX_FRAMES
    assert all(f.via is Via.PROLOGUE for f in frames[1:])


def test_a_caller_that_saved_no_lr_is_not_given_the_live_lr() -> None:
    memory = Memory(
        {
            0x1000: 0xB500,
            0x2000: 0xBF00,
            0x2002: 0xBF00,
            0x2004: 0xF7FF,
            0x2006: 0xFFFE,
            0x3000: 0xF7FF,
            0x3002: 0xFFFE,
        }
    )
    functions = Functions([("callee", 0x1001, 8), ("caller", 0x2001, 0x10), ("outer", 0x3001, 8)])
    sp = 0x2000_0000
    memory.put32(sp, 0x2009)
    memory.put32(sp + 4, 0)
    frames = unwind(memory, functions, 0x1002, 0x3005, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [(0x1002, Via.PC), (0x2008, Via.PROLOGUE)]


def ready_for_calls() -> popcorn.Machine:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(FAULT_ELF))
    m.reset()
    assert asyncio.run(m.run(until=m.sym("ready"), budget=10_000)).reason is (
        popcorn.StopReason.ADDRESS
    )
    return m


def test_a_fault_inside_a_call_reports_the_faulting_frames() -> None:
    m = ready_for_calls()
    caller = {name: int(getattr(m.reg, name)) for name in ("pc", "sp", "lr")}
    result = asyncio.run(m.call("level1", 1, budget=10_000))
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert {name: int(getattr(m.reg, name)) for name in caller} == caller
    report = stop_report(m, result.stop)
    assert report.registers["pc"] == result.stop.pc
    names = [f.location.symbol for f in report.frames[:3]]
    assert names == ["level3", "level2", "level1"]
    assert all(f.exact for f in report.frames[:3])


def test_only_a_fault_inside_a_call_carries_registers() -> None:
    m = ready_for_calls()
    result = asyncio.run(m.call("level1", 1, budget=10_000))
    assert result.stop.registers is not None
    assert result.stop.registers["pc"] == result.stop.pc
    assert m.last_stop == result.stop
    with pytest.raises(TypeError):
        cast(dict[str, int], result.stop.registers)["pc"] = 0
    assert "registers" not in repr(result.stop)
    budget = asyncio.run(m.call("level1", 1, budget=3))
    assert budget.stop.reason is popcorn.StopReason.BUDGET
    assert budget.stop.registers is None
    _, plain = nested()
    assert plain.reason is popcorn.StopReason.FAULT
    assert plain.registers is None


def test_a_handler_fault_inside_a_call_unwinds_through_the_call_stack() -> None:
    m = ready_for_calls()
    m.nvic.pend(0)
    result = asyncio.run(
        m.call(
            "worker",
            budget=10_000,
            interrupts=True,
            stack=popcorn.Stack.PROCESS,
            stack_top=0x200B_0000,
        )
    )
    assert result.stop.reason is popcorn.StopReason.FAULT
    report = stop_report(m, result.stop, ring=False)
    names = [f.location.symbol for f in report.frames[:3]]
    assert names == ["in_handler", "IRQ0_Handler", "worker"]
    assert [f.via for f in report.frames[:3]] == [Via.PC, Via.PROLOGUE, Via.EXCEPTION]


def test_registers_a_stop_lacks_come_from_the_machine() -> None:
    m = ready_for_calls()
    result = asyncio.run(m.call("level1", 1, budget=10_000))
    assert result.stop.registers is not None
    partial = {k: v for k, v in result.stop.registers.items() if k not in ("r12", "psp")}
    report = stop_report(m, dataclasses.replace(result.stop, registers=partial), ring=False)
    assert report.registers["pc"] == result.stop.pc
    assert report.registers["r12"] == m.reg.r12


def test_a_call_through_null_names_the_caller() -> None:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(FAULT_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    result = asyncio.run(m.call("null_call"))
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert result.stop.pc == 0
    report = stop_report(m, result.stop, ring=False)
    assert [(f.location.pc, f.via) for f in report.frames][:1] == [(0, Via.PC)]
    assert [f.location.symbol for f in report.frames][1:2] == ["null_call"]
    assert report.frames[1].via is Via.LR and report.frames[1].exact
    lines = (FAULT_ELF.parent.parent / "fault.c").read_text().splitlines()
    assert "hook();" in lines[report.frames[1].location.line - 1]


def calling_memory() -> tuple[Memory, Functions]:
    """`caller` pushes lr, calls with a BL and returns with a pop. `outer` calls `caller`."""
    memory = Memory({0x2000: 0xB500, 0x2002: 0xF7FF, 0x2004: 0xFFFE, 0x2006: 0xBD00})
    memory.put16(0x3000, 0xF7FF)
    memory.put16(0x3002, 0xFFFE)
    functions = Functions([("caller", 0x2001, 0x10), ("outer", 0x3001, 0x10)])
    return memory, functions


def test_a_pc_outside_every_function_unwinds_from_lr_through_the_caller() -> None:
    memory, functions = calling_memory()
    sp = 0x2000_0000
    memory.put32(sp, 0x3005)
    frames = unwind(memory, functions, 0, 0x2007, sp, 0)
    assert [(f.pc, f.via) for f in frames][:3] == [
        (0, Via.PC),
        (0x2006, Via.LR),
        (0x3004, Via.PROLOGUE),
    ]
    assert source_address(memory, frames[1]) == 0x2004


@pytest.mark.parametrize("lr", [0x2006, 0x9007], ids=["no-thumb-bit", "call-in-no-function"])
def test_a_pc_outside_every_function_with_no_return_lr_is_one_frame(lr: int) -> None:
    memory, functions = calling_memory()
    assert [(f.pc, f.via) for f in unwind(memory, functions, 0, lr, 0x2000_0000, 0)] == [
        (0, Via.PC)
    ]


def test_a_pc_outside_every_function_with_lr_after_no_call_is_scanned() -> None:
    memory, functions = calling_memory()
    memory.put16(0x200A, 0xBF00)
    memory.put16(0x200C, 0xBF00)
    sp = 0x2000_0000
    memory.put32(sp, 0x3005)
    frames = unwind(memory, functions, 0, 0x200F, sp, 0)
    assert [(f.pc, f.via) for f in frames] == [(0, Via.PC), (0x3004, Via.SCAN)]


def test_an_exception_frame_with_a_null_pc_goes_on_from_its_stacked_lr() -> None:
    memory, functions = calling_memory()
    memory.put16(0x1000, 0xB500)
    functions = Functions(
        [("handler", 0x1001, 0x10), ("caller", 0x2001, 0x10), ("outer", 0x3001, 0x10)]
    )
    msp = 0x2000_1000
    memory.put32(msp, 0xFFFF_FFF9)
    frame = msp + 4
    for index, word in enumerate([0, 0, 0, 0, 0, 0x2007, 0, 0x0100_0000]):
        memory.put32(frame + 4 * index, word)
    memory.put32(frame + 32, 0x3005)
    frames = unwind(memory, functions, 0x1002, 0xFFFF_FFF9, msp, 0)
    assert [(f.pc, f.via) for f in frames][:4] == [
        (0x1002, Via.PC),
        (0, Via.EXCEPTION),
        (0x2006, Via.LR),
        (0x3004, Via.PROLOGUE),
    ]


def test_a_null_pc_with_exc_return_in_lr_walks_the_exception_frame() -> None:
    memory, functions = calling_memory()
    sp = 0x2000_0000
    for index, word in enumerate([0, 0, 0, 0, 0, 0, 0x2006, 0x0100_0000]):
        memory.put32(sp + 4 * index, word)
    frames = unwind(memory, functions, 0, 0xFFFF_FFF9, sp, 0)
    assert [(f.pc, f.via) for f in frames][:2] == [(0, Via.PC), (0x2006, Via.EXCEPTION)]
    foreign = unwind(memory, functions, 0, 0xFFFF_FFB9, sp, 0)
    assert [f.via for f in foreign][:1] == [Via.PC]
    assert all(f.via is Via.SCAN for f in foreign[1:])
