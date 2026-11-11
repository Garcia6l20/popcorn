"""LPTIM4 of the STM32U5 and STM32H5 SVDs lists CCMR1 and CCR2, which the RMs reserve."""

import pytest
from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import find_sources

U5_PARTS = ("STM32U575AIIx", "STM32U585AIIx")
H56X_PARTS = ("STM32H562ZITx", "STM32H563ZITx", "STM32H573AIIx")
H5_OTHER_PARTS = ("STM32H503RBTx", "STM32H523CETx", "STM32H533RETx")
CHANNEL_REGISTERS = {"CCMR1", "CCR2"}


def _registers(part: str) -> tuple[dict[str, set[str]], int]:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    description, report = build_description_with_report(sources)
    registers = {
        p.name: {r.name for r in p.registers}
        for p in description.peripherals
        if p.name.startswith(("LPTIM", "SEC_LPTIM"))
    }
    return registers, report.registers_dropped


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", U5_PARTS + H56X_PARTS)
def test_lptim4_has_no_channel_register_and_the_other_lptim_keep_them(part: str) -> None:
    registers, dropped = _registers(part)
    assert dropped == 2
    for name, names in registers.items():
        if name.endswith("LPTIM4"):
            assert not names & CHANNEL_REGISTERS, name
        else:
            assert names >= CHANNEL_REGISTERS, name
    assert {"LPTIM4", "SEC_LPTIM4"} <= registers.keys()


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", H5_OTHER_PARTS)
def test_the_h5_parts_without_lptim4_drop_nothing(part: str) -> None:
    registers, dropped = _registers(part)
    assert dropped == 0
    assert not any(name.endswith("LPTIM4") for name in registers)
    for names in registers.values():
        assert names >= CHANNEL_REGISTERS
