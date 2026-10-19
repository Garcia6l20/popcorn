"""Register-view writes go over the bus as the debugger, `poke` stores raw."""

from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path

import popcorn
import pytest
from popcorn.units import us

from .conftest import DRIVER_ELF, M33_JSON, StorageModel

EXTI_SWIER1 = 0x08


def test_a_register_write_fires_the_exti_software_interrupt(board: popcorn.Machine) -> None:
    exti = board.periph.EXTI
    exti.EXTI_SWIER1.value = 1 << 9
    assert exti.EXTI_RPR1.value & (1 << 9)
    assert exti.EXTI_SWIER1.value == 0


def test_a_field_write_fires_the_exti_software_interrupt(board: popcorn.Machine) -> None:
    exti = board.periph.EXTI
    exti.EXTI_SWIER1.SWI9 = 1
    assert exti.EXTI_RPR1.RPIF9 == 1
    assert exti.EXTI_SWIER1.value == 0


def test_a_field_write_leaves_one_to_clear_siblings_pending(board: popcorn.Machine) -> None:
    pending = board.periph.EXTI.EXTI_RPR1
    pending.poke(RPIF3=1, RPIF9=1)
    pending.RPIF3 = 1
    assert (pending.RPIF3, pending.RPIF9) == (0, 1)


def test_a_field_write_leaves_zero_to_clear_siblings_set(board: popcorn.Machine) -> None:
    board.periph.RCC.RCC_APB1ENR1.TIM2EN = 1
    status = board.periph.TIM2.SR
    status.poke(UIF=1, CC1IF=1)
    status.CC1IF = 0
    assert (status.UIF, status.CC1IF) == (1, 0)


def test_poke_stores_without_effects_and_checks_its_arguments(board: popcorn.Machine) -> None:
    pending = board.periph.EXTI.EXTI_RPR1
    pending.poke(0xFF)
    assert pending.value == 0xFF
    software = board.periph.EXTI.EXTI_SWIER1
    software.poke(SWI0=1, SWI2=1)
    assert software.value == 0b101
    assert pending.value == 0xFF
    with pytest.raises(TypeError):
        software.poke()
    with pytest.raises(TypeError):
        software.poke(1, SWI0=1)
    with pytest.raises(AttributeError):
        software.poke(SWI0=0, NOPE=1)
    with pytest.raises(ValueError, match="does not fit"):
        software.poke(SWI0=0, SWI1=2)
    with pytest.raises(ValueError, match="does not fit"):
        software.poke(1 << 32)
    assert software.value == 0b101


def test_a_register_ref_written_in_a_free_run_reaches_the_model(board: popcorn.Machine) -> None:
    exti = board.periph.EXTI

    async def scenario() -> None:
        async with board.running():
            await board.write((board.ref.periph.EXTI.EXTI_SWIER1, 1 << 9))

    asyncio.run(scenario())
    assert exti.EXTI_RPR1.RPIF9 == 1
    assert exti.EXTI_SWIER1.value == 0


def test_a_brr_write_with_ue_set_takes_effect_and_logs_nothing(
    board: popcorn.Machine, caplog: pytest.LogCaptureFixture
) -> None:
    usart = board.periph.USART1
    assert usart.CR1_enabled.UE == 1
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        usart.BRR.value = 0x123
        asyncio.run(board.run(for_=us(10)))
    assert usart.BRR.value == 0x123
    assert [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "USART1"] == []


def test_a_dmar_write_reaches_its_burst_target_without_advancing(board: popcorn.Machine) -> None:
    board.periph.RCC.RCC_APB1ENR1.TIM5EN = 1
    tim5 = board.periph.TIM5
    tim5.DCR.value = (2 << 8) | 13
    tim5.DMAR.value = 11
    tim5.DMAR.value = 22
    assert (tim5.CCR1.value, tim5.CCR2.value) == (22, 0)


def _described(tmp_path: Path, register: dict[str, object], cross: list[object]) -> popcorn.Machine:
    description = json.loads(M33_JSON.read_text(encoding="utf-8"))
    device = next(p for p in description["peripherals"] if p["name"] == "TESTDEV")
    device["registers"].append(register)
    device["cross_rules"].extend(cross)
    path = tmp_path / "described.json"
    path.write_text(json.dumps(description), encoding="utf-8")
    m = popcorn.Machine(
        popcorn.load_description(path),
        image=popcorn.Image.elf(DRIVER_ELF),
        models={"TESTDEV": StorageModel},
    )
    m.reset()
    return m


def _field(name: str, lsb: int, on_write: str, acc: str = "read_write") -> dict[str, object]:
    return {
        "name": name,
        "lsb": lsb,
        "width": 8,
        "acc": acc,
        "on_write": on_write,
        "on_read": "none",
    }


def _register(*fields: dict[str, object]) -> dict[str, object]:
    return {
        "name": "MIXED",
        "offset": 0x60,
        "width": 32,
        "reset": 0,
        "reset_mask": 0xFFFF_FFFF,
        "acc": "read_write",
        "fields": list(fields),
    }


def _mixed(tmp_path: Path) -> popcorn.Machine:
    return _described(
        tmp_path, _register(_field("KEEP", 0, "modify"), _field("GO", 8, "clear")), []
    )


def test_a_field_write_leaves_toggle_siblings_unchanged(tmp_path: Path) -> None:
    m = _described(
        tmp_path,
        _register(
            _field("KEEP", 0, "modify"),
            _field("FLIP", 8, "zero_to_toggle"),
            _field("TOG", 16, "one_to_toggle"),
        ),
        [],
    )
    mixed = m.periph.TESTDEV.MIXED
    mixed.poke(FLIP=0, TOG=1)
    mixed.KEEP = 1
    assert (mixed.KEEP, mixed.FLIP, mixed.TOG) == (1, 0, 1)


def test_a_field_write_writes_0_to_a_write_only_sibling(tmp_path: Path) -> None:
    m = _described(
        tmp_path,
        _register(_field("KEEP", 0, "modify"), _field("CF", 8, "modify", acc="write_only")),
        [
            {
                "reg": "MIXED",
                "field": "CF",
                "target_reg": "SR",
                "target_field": "FLAG",
                "effect": "clear",
            }
        ],
    )
    device = m.periph.TESTDEV
    device.SR.poke(FLAG=1)
    device.MIXED.poke(CF=1)
    device.MIXED.KEEP = 1
    assert device.SR.FLAG == 1


def test_a_sibling_whose_every_write_acts_logs_once_per_register(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    m = _mixed(tmp_path)
    mixed = m.periph.TESTDEV.MIXED
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        mixed.poke(GO=0x5A)
        mixed.KEEP = 1
        mixed.KEEP = 2
        asyncio.run(m.run(for_=us(10)))
    assert mixed.value == 2
    logged = [r.getMessage() for r in caplog.records if "every write" in r.getMessage()]
    assert len(logged) == 1
    assert "MIXED" in logged[0]
    assert "GO" in logged[0]
