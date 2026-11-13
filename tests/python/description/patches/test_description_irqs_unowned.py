"""`[irqs] add` with and without an owning peripheral."""

from pathlib import Path

import pytest
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._irqs import build_irq_table
from popcorn.stm32.description._patches import (
    IrqAdd,
    PatchIrqs,
    PatchReport,
    PatchTarget,
    apply_patches,
    load_patches,
)
from popcorn.stm32.description._svd import SvdCpu, SvdDevice, SvdPeripheral

from ..conftest import target_from_svd, target_peripheral


def _device() -> SvdDevice:
    peripherals = [
        SvdPeripheral(
            name=name,
            base=0,
            size=0,
            group="",
            derived_from=None,
            registers=[],
            interrupts=interrupts,
        )
        for name, interrupts in (("A", [(5, "FOO")]), ("B", []))
    ]
    return SvdDevice(name="SYNTH", version="1.0", cpu=SvdCpu(), peripherals=peripherals)


def _apply(tmp_path: Path, add: str) -> tuple[PatchTarget, PatchReport]:
    path = tmp_path / "patch.toml"
    path.write_text(
        f'format = 1\n[match]\nparts = ["TOY*"]\n[irqs]\nadd = [{add}]\n', encoding="utf-8"
    )
    target = target_from_svd(_device())
    report = apply_patches(load_patches([path], part="TOY1", svd_name="toy.svd"), target)
    return target, report


def test_an_add_without_a_peripheral_gives_a_number_and_a_name_and_no_owner(
    tmp_path: Path,
) -> None:
    target, report = _apply(tmp_path, '{ number = 9, name = "HSPI1" }')
    assert report.irqs_added == 1
    assert {irq.number for irq in target.irqs} == {5, 9}
    assert target_peripheral(target, "A").irqs == [5]
    assert target_peripheral(target, "B").irqs == []


def test_an_add_with_a_peripheral_gives_it_the_number(tmp_path: Path) -> None:
    target, report = _apply(tmp_path, '{ number = 9, name = "HSPI1", peripheral = "B" }')
    assert report.irqs_added == 1
    assert target_peripheral(target, "B").irqs == [9]


def test_an_add_on_a_used_number_is_refused_without_a_peripheral(tmp_path: Path) -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _apply(tmp_path, '{ number = 5, name = "HSPI1" }')
    assert excinfo.value.expected == "an unused IRQ number"


def test_an_add_on_an_unknown_peripheral_is_still_refused(tmp_path: Path) -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _apply(tmp_path, '{ number = 9, name = "HSPI1", peripheral = "NOPE" }')
    assert excinfo.value.expected == "a peripheral named NOPE"


def test_the_irq_table_lists_an_unowned_add_and_gives_no_peripheral_the_number() -> None:
    patch = PatchIrqs(add=[IrqAdd(number=9, name="HSPI1")])
    table = build_irq_table(_device(), None, [patch], rzone_num=None)
    assert {irq.number for irq in table.irqs} == {5, 9}
    assert all(9 not in numbers for numbers in table.by_peripheral.values())


def test_the_irq_table_refuses_a_duplicate_unowned_number() -> None:
    patch = PatchIrqs(add=[IrqAdd(number=5, name="HSPI1")])
    with pytest.raises(DescriptionError) as excinfo:
        build_irq_table(_device(), None, [patch], rzone_num=None)
    assert excinfo.value.expected == "an unused IRQ number"
