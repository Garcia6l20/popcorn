from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import read_part, read_rzone, rzone_file_name
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._irqs import build_irq_table, read_cmsis_irqs
from popcorn.stm32.description._patches import IrqAdd, PatchIrqs, load_patches
from popcorn.stm32.description._svd import SvdCpu, SvdDevice, SvdPeripheral, expand_svd

DATA_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32"
CMSIS_HEADER = DATA_DIR / "cmsis" / "synthetic_irqs.h"
STM32U5_TOML = (
    Path(__file__).parents[3]
    / "python"
    / "popcorn"
    / "stm32"
    / "description"
    / "patches"
    / "stm32u5.toml"
)
STM32U575_TOML = (
    Path(__file__).parents[3]
    / "python"
    / "popcorn"
    / "stm32"
    / "description"
    / "patches"
    / "stm32u575.toml"
)


def _peripheral(name: str, interrupts: list[tuple[int, str]]) -> SvdPeripheral:
    return SvdPeripheral(
        name=name, base=0, size=0, group="", derived_from=None, registers=[], interrupts=interrupts
    )


def _device(peripherals: list[SvdPeripheral]) -> SvdDevice:
    return SvdDevice(name="SYNTH", version="1.0", cpu=SvdCpu(), peripherals=peripherals)


def test_read_cmsis_irqs_synthetic_header() -> None:
    text = CMSIS_HEADER.read_text(encoding="utf-8")
    result = read_cmsis_irqs(text, file=str(CMSIS_HEADER))
    assert result == {0: "WWDG_IRQn", 1: "PVD_IRQn", 2: "FOO_IRQn", 5: "BAR_IRQn"}


def test_read_cmsis_irqs_without_the_block_is_an_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_cmsis_irqs("enum { A = 1 };", file="broken.h")
    assert excinfo.value.key == "IRQn_Type"


def test_preprocessor_directive_inside_the_enum_body_is_an_error() -> None:
    text = """
    typedef enum
    {
      WWDG_IRQn = 0,
    #if defined(STM32G0B1xx)
      UCPD1_2_IRQn = 8,
    #else
      UCPD1_IRQn = 8,
    #endif
    } IRQn_Type;
    """
    with pytest.raises(DescriptionError) as excinfo:
        read_cmsis_irqs(text, file="g0.h")
    assert excinfo.value.key == "IRQn_Type"


def test_name_aliased_to_another_name_instead_of_a_value_is_an_error() -> None:
    text = """
    typedef enum
    {
      WWDG_IRQn = 0,
      WWDG_ALIAS_IRQn = WWDG_IRQn,
    } IRQn_Type;
    """
    with pytest.raises(DescriptionError) as excinfo:
        read_cmsis_irqs(text, file="alias.h")
    assert excinfo.value.key == "IRQn_Type.WWDG_ALIAS_IRQn"


def test_two_names_at_the_same_number_is_an_error() -> None:
    text = """
    typedef enum
    {
      FOO_IRQn = 0,
      BAR_IRQn = 0,
    } IRQn_Type;
    """
    with pytest.raises(DescriptionError) as excinfo:
        read_cmsis_irqs(text, file="dup.h")
    assert excinfo.value.key == "IRQn_Type[0]"


def test_cmsis_rename_replaces_the_svd_name() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    table = build_irq_table(device, {5: "FOO_RENAMED_IRQn"}, [], rzone_num=None)
    assert len(table.irqs) == 1
    assert table.irqs[0].number == 5
    assert table.irqs[0].name == "FOO_RENAMED_IRQn"


def test_svd_fallback_appends_the_irqn_suffix() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    table = build_irq_table(device, None, [], rzone_num=None)
    assert len(table.irqs) == 1
    assert table.irqs[0].number == 5
    assert table.irqs[0].name == "FOO_IRQn"


def test_drop_removes_the_number_from_the_table_and_every_peripheral() -> None:
    device = _device([_peripheral("A", [(5, "FOO")]), _peripheral("B", [(6, "BAR")])])
    table = build_irq_table(device, None, [PatchIrqs(drop=["FOO"])], rzone_num=None)
    assert {irq.number for irq in table.irqs} == {6}
    assert table.by_peripheral["A"] == []
    assert table.by_peripheral["B"] == [6]


def test_add_appends_a_new_irq_and_peripheral_entry() -> None:
    device = _device([_peripheral("A", [(5, "FOO")]), _peripheral("B", [])])
    add = IrqAdd(number=9, name="NEWIRQ", peripheral="B")
    table = build_irq_table(device, None, [PatchIrqs(add=[add])], rzone_num=None)
    assert {irq.number for irq in table.irqs} == {5, 9}
    new_irq = next(irq for irq in table.irqs if irq.number == 9)
    assert new_irq.name == "NEWIRQ_IRQn"
    assert table.by_peripheral["B"] == [9]


def test_add_takes_the_cmsis_name_when_the_header_has_the_number() -> None:
    device = _device([_peripheral("A", [])])
    add = IrqAdd(number=9, name="NEWIRQ", peripheral="A")
    table = build_irq_table(device, {9: "NEWIRQ_ALT_IRQn"}, [PatchIrqs(add=[add])], rzone_num=None)
    new_irq = next(irq for irq in table.irqs if irq.number == 9)
    assert new_irq.name == "NEWIRQ_ALT_IRQn"


def test_duplicate_number_with_different_names_is_an_error() -> None:
    device = _device([_peripheral("A", [(5, "FOO")]), _peripheral("B", [(5, "OTHER")])])
    with pytest.raises(DescriptionError) as excinfo:
        build_irq_table(device, None, [], rzone_num=None)
    assert excinfo.value.key == "irqs[5]"


def test_cmsis_number_with_no_svd_interrupt_is_an_error() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    with pytest.raises(DescriptionError) as excinfo:
        build_irq_table(device, {5: "FOO_IRQn", 6: "MISSING_IRQn"}, [], rzone_num=None)
    assert excinfo.value.key == "cmsis"


def test_unknown_drop_name_is_an_error() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    with pytest.raises(DescriptionError) as excinfo:
        build_irq_table(device, None, [PatchIrqs(drop=["NOPE"])], rzone_num=None)
    assert excinfo.value.key == "drop.NOPE"


def test_add_on_an_already_used_number_is_an_error() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    add = IrqAdd(number=5, name="DUP", peripheral="A")
    with pytest.raises(DescriptionError) as excinfo:
        build_irq_table(device, None, [PatchIrqs(add=[add])], rzone_num=None)
    assert excinfo.value.key == "add.DUP"


def test_rzone_mismatch_is_a_note_not_an_error() -> None:
    device = _device([_peripheral("A", [(5, "FOO")])])
    table = build_irq_table(device, None, [], rzone_num=99)
    assert table.num_irqs == 6
    assert any("99" in note and "6" in note for note in table.notes)


def test_num_irqs_is_max_plus_one() -> None:
    device = _device([_peripheral("A", [(0, "A"), (10, "B")])])
    table = build_irq_table(device, None, [], rzone_num=None)
    assert table.num_irqs == 11


def _u575_patches(part: str, svd_name: str) -> list[PatchIrqs]:
    loaded = load_patches([STM32U5_TOML, STM32U575_TOML], part=part, svd_name=svd_name)
    return [p.patch.irqs for p in loaded if p.patch.irqs is not None]


def test_u575_irq_table_with_header(
    st_svd_u575: Path, st_cmsis_u575: Path, st_cubemx_db: Path
) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    cmsis = read_cmsis_irqs(st_cmsis_u575.read_text(encoding="utf-8"), file=str(st_cmsis_u575))

    part = read_part(
        (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes(), file="STM32U575QGIx.xml"
    )
    rzone_data = (st_cubemx_db / "mcu" / "memory" / rzone_file_name(part)).read_bytes()
    rzone = read_rzone(rzone_data, file=rzone_file_name(part))

    patches = _u575_patches("STM32U575QGIx", "STM32U575.svd")
    table = build_irq_table(device, cmsis, patches, rzone_num=rzone.processor.num_interrupts)

    assert len(table.irqs) == 121
    assert {irq.number for irq in table.irqs} == set(cmsis)
    assert table.num_irqs == 126
    assert len(table.notes) == 1
    assert "125" in table.notes[0]

    by_number = {irq.number: irq.name for irq in table.irqs}
    assert by_number[29] == "GPDMA1_Channel0_IRQn"
    assert by_number[27] == "IWDG_IRQn"
    assert by_number[61] == "USART1_IRQn"
    assert by_number[125] == "LSECSSD_IRQn"
    for dropped in (97, 108, 109, 132, 133):
        assert dropped not in by_number

    assert table.by_peripheral["USART1"] == [61]
    assert table.by_peripheral["SEC_USART1"] == []
    assert table.by_peripheral["GPDMA1"] == [*range(29, 37), *range(80, 88)]
    assert table.by_peripheral["IWDG"] == [27]
    assert 132 not in table.by_peripheral["EXTI"]
    assert 133 not in table.by_peripheral["EXTI"]
    assert table.by_peripheral["PKA"] == []
    assert table.by_peripheral["SEC_PKA"] == []
    assert table.by_peripheral["OTFDEC1"] == []
    assert table.by_peripheral["OTFDEC2"] == []

    board_external_irqs = {26, 29, 30, 31, 32, 33, 34, 61, 63, 64}
    assert board_external_irqs <= by_number.keys()
    assert by_number[26] == "EXTI15_IRQn"
    assert by_number[63] == "USART3_IRQn"
    assert by_number[64] == "UART4_IRQn"


def test_u575_irq_table_without_header(st_svd_u575: Path) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    patches = _u575_patches("STM32U575QGIx", "STM32U575.svd")
    table = build_irq_table(device, None, patches, rzone_num=None)

    assert len(table.irqs) == 121
    by_number = {irq.number: irq.name for irq in table.irqs}
    assert by_number[27] == "IWDG_IRQn"
    assert all(name.endswith("_IRQn") for name in by_number.values())
