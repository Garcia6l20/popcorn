import os
import statistics
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from xml.etree.ElementTree import Element

import pytest
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import Access, ReadEffect, RegisterDesc, WriteEffect
from popcorn.stm32.description._svd import SvdDevice, SvdPeripheral, expand_svd

DATA_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "svd"


def _load(name: str) -> SvdDevice:
    data = (DATA_DIR / name).read_bytes()
    return expand_svd(data, file=name)


def _peripheral(device: SvdDevice, name: str) -> SvdPeripheral:
    for peripheral in device.peripherals:
        if peripheral.name == name:
            return peripheral
    raise KeyError(name)


def _register(peripheral: SvdPeripheral, name: str) -> RegisterDesc:
    for register in peripheral.registers:
        if register.name == name:
            return register
    raise KeyError(name)


def _xml_text(el: Element, tag: str) -> str | None:
    child = el.find(tag)
    if child is None or child.text is None:
        return None
    text = child.text.strip()
    return text or None


def test_synthetic_device_and_cpu() -> None:
    device = _load("synthetic.svd")
    assert device.name == "SYNTH"
    assert device.version == "1.0"
    assert device.cpu.name == "CM33"
    assert device.cpu.revision == "r0p1"
    assert device.cpu.fpu_present is True
    assert device.cpu.fpu_dp is False
    assert device.cpu.mpu_present is True
    assert device.cpu.dsp_present is False
    assert device.cpu.nvic_prio_bits == 3
    assert device.cpu.vendor_systick_config is False
    assert device.cpu.sau_num_regions == 0


def test_synthetic_peripheral_a_has_ten_registers_at_the_i05_addresses() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    assert a.base == 0x40000000
    assert a.size == 0x400
    assert a.derived_from is None
    assert len(a.registers) == 10
    offsets = {r.name: r.offset for r in a.registers}
    assert offsets == {
        "SR": 0x0,
        "SR2": 0x4,
        "ARR0": 0x10,
        "ARR1": 0x14,
        "ARR2": 0x18,
        "ARR3": 0x1C,
        "CH0_CR": 0x100,
        "CH1_CR": 0x120,
        "CH0_SUB_DR": 0x110,
        "CH1_SUB_DR": 0x130,
    }


def test_synthetic_peripheral_a_interrupt_from_leading_zero_decimal() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    assert a.interrupts == [(3, "A_IRQ")]


def test_synthetic_binary_reset_value_with_dont_care_bit() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    sr = _register(a, "SR")
    assert sr.reset == 0b100


def test_synthetic_register_derived_from_keeps_fields_and_overrides_own_tags() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    sr = _register(a, "SR")
    sr2 = _register(a, "SR2")
    assert [f.name for f in sr2.fields] == [f.name for f in sr.fields]
    assert sr2.offset == 0x4
    assert sr2.alternate_of is None


def test_synthetic_no_access_anywhere_defaults_to_read_write() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    dr = _register(a, "CH0_SUB_DR")
    assert dr.acc == Access.READ_WRITE
    assert dr.fields == []


def test_synthetic_field_write_and_read_effects() -> None:
    device = _load("synthetic.svd")
    a = _peripheral(device, "A")
    sr = _register(a, "SR")
    by_name = {f.name: f for f in sr.fields}
    assert by_name["F"].on_write == WriteEffect.ONE_TO_CLEAR
    assert by_name["F"].on_read == ReadEffect.NONE
    assert by_name["R"].on_write == WriteEffect.MODIFY
    assert by_name["R"].on_read == ReadEffect.CLEAR


def test_synthetic_peripheral_derived_from_copies_registers_not_interrupt() -> None:
    device = _load("synthetic.svd")
    b = _peripheral(device, "B")
    assert b.base == 0x40001000
    assert b.derived_from == "A"
    assert b.interrupts == []
    assert len(b.registers) == 10
    assert {r.name for r in b.registers} == {r.name for r in _peripheral(device, "A").registers}


def test_synthetic_alternate_group_links_to_the_first_register_at_the_offset() -> None:
    device = _load("synthetic.svd")
    c = _peripheral(device, "C")
    view_a = _register(c, "VIEW_A")
    view_b = _register(c, "VIEW_B")
    assert view_a.alternate_of is None
    assert view_b.alternate_of == "VIEW_A"


def test_synthetic_unlinked_overlaps_is_empty() -> None:
    device = _load("synthetic.svd")
    assert device.unlinked_overlaps() == []


def test_derived_from_cycle_is_a_description_error_naming_the_chain() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _load("cycle.svd")
    assert "X" in excinfo.value.expected
    assert "Y" in excinfo.value.expected


def test_unknown_access_string_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _load("bad_access.svd")
    assert excinfo.value.key == "access"


def test_missing_base_address_is_a_description_error_naming_it() -> None:
    data = b"""<?xml version="1.0"?>
    <device>
      <name>NOBASE</name>
      <version>1.0</version>
      <peripherals>
        <peripheral>
          <name>A</name>
        </peripheral>
      </peripherals>
    </device>
    """
    with pytest.raises(DescriptionError) as excinfo:
        expand_svd(data, file="nobase.svd")
    assert excinfo.value.key == "peripheral[A].baseAddress"


def test_malformed_xml_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        expand_svd(b"<device><name>X</name>", file="broken.svd")
    assert excinfo.value.key == "<root>"


@pytest.fixture(scope="session")
def u575_device(st_svd_u575: Path) -> SvdDevice:
    return expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))


def test_u575_peripheral_register_and_field_counts(u575_device: SvdDevice) -> None:
    registers = [r for p in u575_device.peripherals for r in p.registers]
    fields = [f for r in registers for f in r.fields]
    assert len(u575_device.peripherals) == 198
    assert len(registers) == 4717
    assert len(fields) == 48119


def test_u575_no_field_declares_a_write_or_read_effect(u575_device: SvdDevice) -> None:
    fields = [f for p in u575_device.peripherals for r in p.registers for f in r.fields]
    assert all(f.on_write == WriteEffect.MODIFY for f in fields)
    assert all(f.on_read == ReadEffect.NONE for f in fields)


def test_u575_interrupts_125_distinct_values(u575_device: SvdDevice) -> None:
    values = {value for p in u575_device.peripherals for value, _ in p.interrupts}
    assert len(values) == 125


def test_u575_sec_peripherals_declare_no_interrupt_of_their_own_except_sec_flash(
    u575_device: SvdDevice,
) -> None:
    for peripheral in u575_device.peripherals:
        if peripheral.name.startswith("SEC_") and peripheral.name != "SEC_FLASH":
            assert peripheral.interrupts == []
    sec_flash = _peripheral(u575_device, "SEC_FLASH")
    assert sec_flash.interrupts == [(7, "FLASH_S")]


def test_u575_unlinked_overlaps_are_accounted_for(u575_device: SvdDevice) -> None:
    overlaps = u575_device.unlinked_overlaps()
    assert overlaps == []


def test_u575_611_declared_registers_without_access_default_to_read_write(
    st_svd_u575: Path, u575_device: SvdDevice
) -> None:
    root = ET.fromstring(st_svd_u575.read_bytes())
    device_access = _xml_text(root, "access")
    peripherals_el = root.find("peripherals")
    assert peripherals_el is not None

    absent: set[tuple[str, str]] = set()
    for p in peripherals_el.findall("peripheral"):
        pname = _xml_text(p, "name")
        assert pname is not None
        peripheral_access = _xml_text(p, "access")
        registers_el = p.find("registers")
        if registers_el is None:
            continue
        for reg in registers_el.findall("register"):
            reg_access = _xml_text(reg, "access")
            if device_access is None and peripheral_access is None and reg_access is None:
                reg_name = _xml_text(reg, "name")
                assert reg_name is not None
                absent.add((pname, reg_name))

    assert len(absent) == 611
    by_key = {(p.name, r.name): r for p in u575_device.peripherals for r in p.registers}
    for key in absent:
        assert by_key[key].acc == Access.READ_WRITE


def test_u575_spot_checks(u575_device: SvdDevice) -> None:
    usart1 = _peripheral(u575_device, "USART1")
    assert usart1.base == 0x40013800
    usart2 = _peripheral(u575_device, "USART2")
    assert (62, "USART2") in usart2.interrupts
    dbgmcu = _peripheral(u575_device, "DBGMCU")
    idcode = _register(dbgmcu, "IDCODE")
    assert idcode.reset == 0x30076482


@pytest.mark.skipif(
    os.environ.get("POPCORN_PERF") != "1",
    reason="set POPCORN_PERF=1 to run the expand_svd timing benchmark",
)
def test_u575_expand_time(st_svd_u575: Path) -> None:
    data = st_svd_u575.read_bytes()
    durations: list[float] = []
    for _ in range(10):
        start = time.perf_counter()
        expand_svd(data, file=str(st_svd_u575))
        durations.append(time.perf_counter() - start)
    median = statistics.median(durations)
    print(f"\nexpand_svd(U575) median of 10: {median * 1000:.1f} ms")
    assert median < 2.0
