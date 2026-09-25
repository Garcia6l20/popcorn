import os
import subprocess
from pathlib import Path

import pytest
from popcorn.stm32.description._model import Access, FieldDesc, RegisterDesc
from popcorn.stm32.description._svd import SvdDevice, expand_svd

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_SVD_ORACLE") is None,
    reason="set POPCORN_SVD_ORACLE=<built oracle binary> to cross-check against svd-parser 0.14",
)


def _run_oracle(svd_path: Path) -> list[str]:
    binary = os.environ["POPCORN_SVD_ORACLE"]
    result = subprocess.run(
        [binary, str(svd_path)], capture_output=True, text=True, check=True, timeout=60
    )
    return result.stdout.splitlines()


def _oracle_peripheral(name: str, base: int, size: int) -> tuple[str, int, int]:
    return (name, base, size)


def _oracle_register(
    periph: str,
    name: str,
    offset: int,
    width: int,
    access: str,
    reset: int,
    mask: int,
    nfields: int,
    alternate_of: str,
) -> tuple[str, str, int, int, str, int, int, int, str | None]:
    return (
        periph,
        name,
        offset,
        width,
        access,
        reset,
        mask,
        nfields,
        None if alternate_of == "-" else alternate_of,
    )


def _oracle_field(
    periph: str, reg: str, name: str, lsb: int, width: int, access: str, on_write: str, on_read: str
) -> tuple[str, str, str, int, int, str, str, str]:
    return (periph, reg, name, lsb, width, access, on_write, on_read)


def _parse_oracle_output(lines: list[str]) -> tuple[
    set[tuple[str, int, int]],
    set[tuple[str, str, int, int, str, int, int, int, str | None]],
    set[tuple[str, str, str, int, int, str, str, str]],
    dict[str, set[int]],
]:
    peripherals: set[tuple[str, int, int]] = set()
    registers: set[tuple[str, str, int, int, str, int, int, int, str | None]] = set()
    fields: set[tuple[str, str, str, int, int, str, str, str]] = set()
    irqs_by_peripheral: dict[str, set[int]] = {}

    for line in lines:
        if not line:
            continue
        kind, rest = line.split(" ", 1)
        parts = rest.split(" ")
        if kind == "P":
            name, base, size = parts
            peripherals.add(_oracle_peripheral(name, int(base), int(size)))
        elif kind == "R":
            periph, name, offset, width, access, reset, mask, nfields, alt = parts
            registers.add(
                _oracle_register(
                    periph,
                    name,
                    int(offset),
                    int(width),
                    access,
                    int(reset),
                    int(mask),
                    int(nfields),
                    alt,
                )
            )
        elif kind == "F":
            periph, reg, name, lsb, width, access, on_write, on_read = parts
            fields.add(
                _oracle_field(periph, reg, name, int(lsb), int(width), access, on_write, on_read)
            )
        elif kind == "I":
            number, name = rest.split(" ", 1)
            irqs_by_peripheral.setdefault(name, set()).add(int(number))

    return peripherals, registers, fields, irqs_by_peripheral


def _builder_register_tuple(
    periph: str, r: RegisterDesc
) -> tuple[str, str, int, int, str, int, int, int, str | None]:
    return (
        periph,
        r.name,
        r.offset,
        r.width,
        _access_name(r.acc),
        r.reset,
        r.reset_mask,
        len(r.fields),
        r.alternate_of,
    )


def _builder_field_tuple(
    periph: str, reg: str, f: FieldDesc
) -> tuple[str, str, str, int, int, str, str, str]:
    return (
        periph,
        reg,
        f.name,
        f.lsb,
        f.width,
        _access_name(f.acc),
        f.on_write.value,
        f.on_read.value,
    )


def _access_name(access: Access) -> str:
    return access.value


def test_oracle_matches_peripherals_registers_and_fields(st_svd_u575: Path) -> None:
    device: SvdDevice = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    lines = _run_oracle(st_svd_u575)
    oracle_peripherals, oracle_registers, oracle_fields, _ = _parse_oracle_output(lines)

    builder_peripherals = {_oracle_peripheral(p.name, p.base, p.size) for p in device.peripherals}
    builder_registers = {
        _builder_register_tuple(p.name, r) for p in device.peripherals for r in p.registers
    }
    builder_fields = {
        _builder_field_tuple(p.name, r.name, f)
        for p in device.peripherals
        for r in p.registers
        for f in r.fields
    }

    assert oracle_peripherals == builder_peripherals
    assert oracle_registers == builder_registers
    assert oracle_fields == builder_fields


def test_oracle_matches_irq_number_sets(st_svd_u575: Path) -> None:
    device: SvdDevice = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    lines = _run_oracle(st_svd_u575)
    _, _, _, oracle_irqs = _parse_oracle_output(lines)

    builder_irqs: dict[str, set[int]] = {}
    for p in device.peripherals:
        for number, name in p.interrupts:
            builder_irqs.setdefault(name, set()).add(number)

    assert oracle_irqs == builder_irqs
