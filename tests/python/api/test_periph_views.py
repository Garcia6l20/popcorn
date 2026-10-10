"""Peripheral registers by name."""

from __future__ import annotations

import popcorn
import pytest
from popcorn import _core

from .conftest import Machines

TESTDEV = 0x4001_0000


def test_fields_read_and_write_as_bit_slices(machines: Machines) -> None:
    m = machines()
    register = m.periph.TESTDEV.W_MODIFY
    register.F = 0x5A
    assert register.F == 0x5A
    assert register.value == 0x5A
    register.value = 0x1234_5600 | 0x7F
    assert register.F == 0x7F
    assert m.periph["TESTDEV"]["W_MODIFY"].value == 0x1234_567F
    register.F = 0x11
    assert register.value == 0x1234_5611


def test_writes_land_in_the_register_file(machines: Machines) -> None:
    m = machines()
    m.periph.TESTDEV.RW.VAL = 0xCAFE_F00D
    assert m.periph.TESTDEV.RW.value == 0xCAFE_F00D
    m.periph.TESTDEV.ALT_B.LOW = 0xBEEF
    assert m.periph.TESTDEV.ALT_A.VAL & 0xFFFF == 0xBEEF


def test_names_list_for_completion(machines: Machines) -> None:
    m = machines()
    assert "TESTDEV" in m.periph.names()
    assert "W_MODIFY" in m.periph.TESTDEV.registers
    assert m.periph.TESTDEV.ICR.fields == ("CF",)


def test_unknown_names_and_wide_values_are_refused(machines: Machines) -> None:
    m = machines()
    with pytest.raises(AttributeError, match="no peripheral"):
        _ = m.periph.NOPE
    with pytest.raises(AttributeError, match="no register"):
        _ = m.periph.TESTDEV.NOPE
    with pytest.raises(AttributeError, match="no field"):
        _ = m.periph.TESTDEV.ICR.NOPE
    with pytest.raises(ValueError, match="does not fit"):
        m.periph.TESTDEV.ICR.CF = 2
    with pytest.raises(ValueError, match="does not fit"):
        m.periph.TESTDEV.RW.value = 1 << 32


def test_sub_word_registers_keep_their_neighbours(machines: Machines) -> None:
    m = machines()
    m.periph.TESTDEV.RW.value = 0x1122_3344
    handle = _core.MachineHandle(m.description, _core.MachineConfig())
    handle.poke("TESTDEV", 8, 32, 0x1122_3344)
    handle.poke("TESTDEV", 9, 8, 0xAB)
    handle.poke("TESTDEV", 10, 16, 0xBEEF)
    assert handle.peek("TESTDEV", 8, 32) == 0xBEEF_AB44
    assert handle.peek("TESTDEV", 9, 8) == 0xAB
    with pytest.raises(popcorn.ConfigError):
        handle.peek("TESTDEV", 9, 16)
    with pytest.raises(popcorn.ConfigError):
        handle.peek("NOPE", 0, 32)


def test_bus_reads_and_writes_apply_the_register_effects(machines: Machines) -> None:
    m = machines()
    clear = m.periph.TESTDEV.W_ONE_TO_CLEAR
    clear.poke(0xFF)
    clear.write(0x0F)
    assert clear.value == 0xF0
    assert m.mem.u32[TESTDEV + clear.offset] == 0xF0
    on_read = m.periph.TESTDEV.R_CLEAR
    on_read.poke(0x55)
    assert on_read.value == 0x55
    assert on_read.read() == 0x55
    assert on_read.value == 0
