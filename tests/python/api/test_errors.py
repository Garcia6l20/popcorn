"""Native errors reach Python as the popcorn hierarchy, with their context fields."""

from __future__ import annotations

from pathlib import Path

import popcorn
import pytest


def test_unknown_symbol_is_a_symbol_error_with_close_matches(basic: popcorn.Image) -> None:
    with pytest.raises(popcorn.SymbolError) as caught:
        basic.lookup("Reset_Handlr")
    assert caught.value.name == "Reset_Handlr"
    assert "Reset_Handler" in caught.value.close_matches


def test_bad_elf_is_an_image_error(tmp_path: Path) -> None:
    path = tmp_path / "bad.elf"
    path.write_bytes(b"not an elf")
    with pytest.raises(popcorn.ImageError) as caught:
        popcorn.Image.elf(path)
    assert caught.value.segment == "header"


def test_an_image_error_thrown_in_another_extension_is_translated_too(tmp_path: Path) -> None:
    path = tmp_path / "bad.elf"
    path.write_bytes(b"not an elf")
    with pytest.raises(popcorn.ImageError) as caught:
        popcorn.coverage.ImageRef.from_elf(path)
    assert caught.value.segment == "header"


def test_image_outside_memory_is_an_image_error_naming_the_address(
    tmp_path: Path, m33: popcorn.MachineDescription
) -> None:
    path = tmp_path / "far.bin"
    path.write_bytes(bytes(16))
    with pytest.raises(popcorn.ImageError) as caught:
        popcorn.Machine(m33, image=popcorn.Image.bin(path, 0x3000_0000))
    assert caught.value.address == 0x3000_0000
    assert caught.value.segment == "far.bin"


def test_bad_description_is_a_config_error_with_file_and_key() -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.parse_description("{}", "bad.json")
    assert caught.value.file == "bad.json"
    assert caught.value.key == "schema_version"
    assert caught.value.expected


def test_missing_description_file_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(popcorn.ConfigError):
        popcorn.load_description(tmp_path / "absent.json")


@pytest.mark.parametrize(
    "error",
    [
        popcorn.ConfigError,
        popcorn.ImageError,
        popcorn.SymbolError,
        popcorn.EmulationError,
        popcorn.FaultError,
        popcorn.BudgetExceeded,
        popcorn.ModelError,
        popcorn.MachineRunning,
        popcorn.MachineBusy,
    ],
)
def test_every_error_is_a_popcorn_error(error: type[Exception]) -> None:
    assert issubclass(error, popcorn.PopcornError)


def test_fault_and_budget_errors_are_emulation_errors() -> None:
    fault = popcorn.FaultError("f", kind=popcorn.FaultKind.BUS, pc=0x100)
    budget = popcorn.BudgetExceeded("b", budget=10)
    assert isinstance(fault, popcorn.EmulationError)
    assert fault.reason is popcorn.StopReason.FAULT
    assert budget.reason is popcorn.StopReason.BUDGET


def test_reset_with_no_vector_table_is_an_emulation_error(
    m33: popcorn.MachineDescription, basic: popcorn.Image
) -> None:
    m = popcorn.Machine(m33, image=basic, vector_table=0x3000_0000)
    with pytest.raises(popcorn.EmulationError) as caught:
        m.reset()
    assert "no vector table" in str(caught.value)
    assert caught.value.reason is None
    assert isinstance(caught.value, popcorn.PopcornError)
