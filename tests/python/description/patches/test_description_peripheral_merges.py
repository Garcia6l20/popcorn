"""`[[peripheral_merges]]`: an SVD that splits one block into several peripherals, folded back."""

import copy
from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._irqs import build_irq_table
from popcorn.stm32.description._patches import (
    IrqAdd,
    PatchIrqs,
    PatchReport,
    PatchTarget,
    apply_patches,
    load_patches,
    peripheral_owners,
)
from popcorn.stm32.description._sources import find_sources
from popcorn.stm32.description._svd import SvdDevice, expand_svd

from ..conftest import target_from_svd, target_peripheral


def _register(name: str, offset: int) -> str:
    return (
        f"<register><name>{name}</name><addressOffset>{offset:#x}</addressOffset>"
        "<fields><field><name>V</name><bitOffset>0</bitOffset><bitWidth>8</bitWidth></field>"
        "</fields></register>"
    )


def _peripheral(name: str, base: int, registers: list[tuple[str, int]], irq: int | None) -> str:
    interrupt = (
        f"<interrupt><name>{name}</name><value>{irq}</value></interrupt>" if irq is not None else ""
    )
    return (
        f"<peripheral><name>{name}</name><groupName>USB_OTG_FS</groupName>"
        f"<baseAddress>{base:#x}</baseAddress>"
        "<addressBlock><offset>0</offset><size>0x400</size><usage>registers</usage></addressBlock>"
        f"{interrupt}<registers>{''.join(_register(n, o) for n, o in registers)}</registers>"
        "</peripheral>"
    )


type _Blocks = dict[str, tuple[int, list[tuple[str, int]], int | None]]

_BLOCKS: _Blocks = {
    "OTG_FS_GLOBAL": (0x5000_0000, [("FS_GOTGCTL", 0x0), ("FS_GUSBCFG", 0xC)], 67),
    "OTG_FS_HOST": (0x5000_0400, [("FS_HCFG", 0x0), ("HFIR", 0x4)], 68),
    "OTG_FS_DEVICE": (0x5000_0800, [("FS_DCFG", 0x0), ("FS_DCTL", 0x4)], None),
    "OTG_FS_PWRCLK": (0x5000_0E00, [("FS_PCGCCTL", 0x0)], None),
    "TIM2": (0x4000_0000, [("CR1", 0x0)], 28),
}


def _svd(blocks: _Blocks | None = None) -> bytes:
    peripherals = "".join(
        _peripheral(name, base, registers, irq)
        for name, (base, registers, irq) in (blocks or _BLOCKS).items()
    )
    return (
        '<?xml version="1.0" encoding="utf-8"?><device schemaVersion="1.3"><name>TOY</name>'
        "<cpu><name>CM4</name><revision>r0p1</revision><fpuPresent>true</fpuPresent>"
        "<mpuPresent>true</mpuPresent><nvicPrioBits>4</nvicPrioBits>"
        "<vendorSystickConfig>false</vendorSystickConfig></cpu>"
        "<addressUnitBits>8</addressUnitBits><width>32</width><size>32</size>"
        "<resetValue>0</resetValue><resetMask>0xFFFFFFFF</resetMask>"
        f"<peripherals>{peripherals}</peripherals></device>"
    ).encode()


def _device(
    blocks: _Blocks | None = None,
) -> SvdDevice:
    return expand_svd(_svd(blocks), file="toy.svd")


def _target(
    blocks: _Blocks | None = None,
) -> PatchTarget:
    return target_from_svd(_device(blocks))


_MERGE = """
[[peripheral_merges]]
into = "{into}"
peripherals = [{merged}]
size = {size}
source = "RM0351 47.15.57 Table 333"
"""

_FOUR = '"OTG_FS_HOST", "OTG_FS_DEVICE", "OTG_FS_PWRCLK"'


def _write(tmp_path: Path, body: str, name: str = "patch.toml", parts: str = '"TOY*"') -> Path:
    path = tmp_path / name
    path.write_text(f"format = 1\n[match]\nparts = [{parts}]\n{body}", encoding="utf-8")
    return path


def _merge(into: str = "OTG_FS_GLOBAL", merged: str = _FOUR, size: str = "0x20000") -> str:
    return _MERGE.format(into=into, merged=merged, size=size)


def _apply(target: PatchTarget, *paths: Path, part: str = "TOY1") -> PatchReport:
    return apply_patches(load_patches(list(paths), part=part, svd_name="toy.svd"), target)


def test_a_merge_moves_registers_by_the_base_distance_and_removes_the_merged_blocks(
    tmp_path: Path,
) -> None:
    target = _target()
    report = _apply(target, _write(tmp_path, _merge()))
    merged = target_peripheral(target, "OTG_FS_GLOBAL")
    assert [(r.name, r.offset) for r in merged.registers] == [
        ("FS_GOTGCTL", 0x0),
        ("FS_GUSBCFG", 0xC),
        ("FS_HCFG", 0x400),
        ("HFIR", 0x404),
        ("FS_DCFG", 0x800),
        ("FS_DCTL", 0x804),
        ("FS_PCGCCTL", 0xE00),
    ]
    assert (merged.base, merged.size) == (0x5000_0000, 0x20000)
    assert merged.base % 0x400 == 0
    assert [p.name for p in target.peripherals] == ["OTG_FS_GLOBAL", "TIM2"]
    assert all(p.base != 0x5000_0E00 for p in target.peripherals)
    assert report.peripherals_merged == 3


def test_a_merge_moves_the_irqs_of_the_merged_blocks(tmp_path: Path) -> None:
    target = _target()
    _apply(target, _write(tmp_path, _merge()))
    assert target_peripheral(target, "OTG_FS_GLOBAL").irqs == [67, 68]
    assert target_peripheral(target, "TIM2").irqs == [28]


def test_a_merge_keeps_the_fields_of_the_moved_registers(tmp_path: Path) -> None:
    target = _target()
    _apply(target, _write(tmp_path, _merge()))
    registers = {r.name: r for r in target_peripheral(target, "OTG_FS_GLOBAL").registers}
    assert [(f.name, f.lsb, f.width) for f in registers["FS_DCFG"].fields] == [("V", 0, 8)]


@pytest.mark.parametrize(
    ("body", "key", "expected"),
    [
        (_merge(into="OTG_FS"), "peripheral_merges[0].into", "a peripheral of the description"),
        (
            _merge(merged='"OTG_FS_HOST", "USB"'),
            "peripheral_merges[0].peripherals.USB",
            "a peripheral of the description",
        ),
        (
            _merge(merged='"OTG_FS_HOST", "OTG_FS_HOST"'),
            "peripheral_merges[0].peripherals.OTG_FS_HOST",
            "each peripheral once, other than OTG_FS_GLOBAL",
        ),
        (
            _merge(merged='"OTG_FS_GLOBAL"'),
            "peripheral_merges[0].peripherals.OTG_FS_GLOBAL",
            "each peripheral once, other than OTG_FS_GLOBAL",
        ),
        (
            _merge(merged='"TIM2"'),
            "peripheral_merges[0].peripherals.TIM2",
            "a base at or above 0x50000000",
        ),
        (
            _merge(merged=""),
            "peripheral_merges[0].peripherals",
            "at least one peripheral to merge",
        ),
        (
            _merge(size="0x200"),
            "peripheral_merges[0].size",
            "a size of at least 0x400",
        ),
        (
            _merge(size="0xE00"),
            "peripheral_merges[0].size",
            "a size covering FS_PCGCCTL at 0xe00",
        ),
    ],
    ids=[
        "unknown-into",
        "unknown-merged",
        "merged-twice",
        "merged-is-into",
        "base-below-into",
        "empty-list",
        "size-below-into",
        "register-past-size",
    ],
)
def test_a_bad_merge_is_refused_and_leaves_the_target_whole(
    tmp_path: Path, body: str, key: str, expected: str
) -> None:
    target = _target()
    before = copy.deepcopy(target)
    with pytest.raises(DescriptionError) as error:
        _apply(target, _write(tmp_path, body))
    assert (error.value.key, error.value.expected) == (key, expected)
    assert target == before


def test_a_register_name_used_by_another_merged_block_is_refused(tmp_path: Path) -> None:
    blocks = dict(_BLOCKS)
    blocks["OTG_FS_DEVICE"] = (0x5000_0800, [("FS_DCFG", 0x0), ("HFIR", 0x4)], None)
    with pytest.raises(DescriptionError) as error:
        _apply(_target(blocks), _write(tmp_path, _merge()))
    assert error.value.key == "peripheral_merges[0].peripherals.OTG_FS_DEVICE"
    assert error.value.expected == "register names not in use, not HFIR"


def test_a_register_name_of_into_is_refused(tmp_path: Path) -> None:
    blocks = dict(_BLOCKS)
    blocks["OTG_FS_HOST"] = (0x5000_0400, [("FS_HCFG", 0x0), ("FS_GUSBCFG", 0x4)], None)
    with pytest.raises(DescriptionError) as error:
        _apply(_target(blocks), _write(tmp_path, _merge()))
    assert error.value.key == "peripheral_merges[0].peripherals.OTG_FS_HOST"
    assert error.value.expected == "register names not in use, not FS_GUSBCFG"


def test_two_moved_registers_on_one_address_are_refused(tmp_path: Path) -> None:
    blocks = dict(_BLOCKS)
    blocks["OTG_FS_PWRCLK"] = (0x5000_0800, [("FS_PCGCCTL", 0x4)], None)
    with pytest.raises(DescriptionError) as error:
        _apply(_target(blocks), _write(tmp_path, _merge()))
    assert error.value.key == "peripheral_merges[0].peripherals.OTG_FS_PWRCLK"
    assert error.value.expected == "FS_PCGCCTL clear of FS_DCTL"


def test_a_moved_register_over_a_register_of_into_is_refused(tmp_path: Path) -> None:
    blocks = dict(_BLOCKS)
    blocks["OTG_FS_GLOBAL"] = (0x5000_0000, [("FS_GOTGCTL", 0x0), ("FS_GUSBCFG", 0x404)], 67)
    with pytest.raises(DescriptionError) as error:
        _apply(_target(blocks), _write(tmp_path, _merge()))
    assert error.value.key == "peripheral_merges[0].peripherals.OTG_FS_HOST"
    assert error.value.expected == "HFIR clear of FS_GUSBCFG"


def test_a_merge_names_into_as_the_peripheral_renames_leave_it(tmp_path: Path) -> None:
    target = _target()
    rename = '[peripherals]\nrenames = { OTG_FS_GLOBAL = "OTG_FS" }\nsource = "RM0351 47.15.57"\n'
    _apply(target, _write(tmp_path, rename + _merge(into="OTG_FS")))
    assert [p.name for p in target.peripherals] == ["OTG_FS", "TIM2"]
    assert len(target_peripheral(target, "OTG_FS").registers) == 7


def test_a_merge_naming_into_by_its_name_before_the_rename_is_refused(tmp_path: Path) -> None:
    target = _target()
    rename = '[peripherals]\nrenames = { OTG_FS_GLOBAL = "OTG_FS" }\nsource = "RM0351 47.15.57"\n'
    with pytest.raises(DescriptionError) as error:
        _apply(target, _write(tmp_path, rename + _merge()))
    assert error.value.key == "peripheral_merges[0].into"


def test_a_later_rule_names_the_moved_register_in_into(tmp_path: Path) -> None:
    target = _target()
    later = (
        '[[register_renames]]\nperipherals = ["OTG_FS_GLOBAL"]\nrenames = { HFIR = "FS_HFIR" }\n'
        'source = "RM0351 47.15.57"\n'
    )
    _apply(target, _write(tmp_path, _merge() + later))
    names = {r.name for r in target_peripheral(target, "OTG_FS_GLOBAL").registers}
    assert "FS_HFIR" in names


_L471_DROP = (
    "[peripherals]\n"
    f'drop = ["OTG_FS_GLOBAL", {_FOUR}]\n'
    'source = "DS10741 Table 2: no USB OTG FS"\n'
)


def _line_patches(tmp_path: Path, family_merge: bool = False) -> list[Path]:
    merge_parts = '"TOY47*"' if family_merge else '"TOY475*", "TOY476*"'
    return [
        _write(tmp_path, _L471_DROP, "toy471.toml", '"TOY471*"'),
        _write(tmp_path, _merge(), "toy_otg.toml", merge_parts),
    ]


def test_the_otg_lines_merge_and_the_l471_drops_the_four_blocks(tmp_path: Path) -> None:
    paths = _line_patches(tmp_path)
    l476 = _target()
    _apply(l476, *paths, part="TOY476RCTx")
    assert [p.name for p in l476.peripherals] == ["OTG_FS_GLOBAL", "TIM2"]
    l471 = _target()
    report = _apply(l471, *paths, part="TOY471RGTx")
    assert [p.name for p in l471.peripherals] == ["TIM2"]
    assert (report.peripherals_dropped, report.peripherals_merged) == (4, 0)


def test_a_merge_in_a_patch_matching_the_l471_is_refused_after_its_drop(tmp_path: Path) -> None:
    paths = _line_patches(tmp_path, family_merge=True)
    with pytest.raises(DescriptionError) as error:
        _apply(_target(), *paths, part="TOY471RGTx")
    assert (error.value.key, error.value.expected) == (
        "peripheral_merges[0].into",
        "a peripheral of the description",
    )


def test_owners_follow_drops_renames_and_merges(tmp_path: Path) -> None:
    rename = (
        '[peripherals]\nrenames = { OTG_FS_GLOBAL = "OTG_FS", OTG_FS_HOST = "HOST" }\n'
        'drop = ["TIM2"]\nsource = "RM0351 47.15.57"\n'
    )
    path = _write(tmp_path, rename + _merge(into="OTG_FS", merged='"HOST", "OTG_FS_DEVICE"'))
    loaded = load_patches([path], part="TOY1", svd_name="toy.svd")
    apply_patches(loaded, _target())
    assert peripheral_owners(loaded) == {
        "OTG_FS_GLOBAL": "OTG_FS",
        "OTG_FS_HOST": "OTG_FS",
        "OTG_FS_DEVICE": "OTG_FS",
        "TIM2": None,
    }


def test_a_chained_merge_moves_the_first_merge_to_the_last_into(tmp_path: Path) -> None:
    body = _merge(into="OTG_FS_DEVICE", merged='"OTG_FS_PWRCLK"') + _merge(
        merged='"OTG_FS_HOST", "OTG_FS_DEVICE"'
    )
    loaded = load_patches([_write(tmp_path, body)], part="TOY1", svd_name="toy.svd")
    target = _target()
    apply_patches(loaded, target)
    assert [p.name for p in target.peripherals] == ["OTG_FS_GLOBAL", "TIM2"]
    assert peripheral_owners(loaded) == {
        "OTG_FS_PWRCLK": "OTG_FS_GLOBAL",
        "OTG_FS_DEVICE": "OTG_FS_GLOBAL",
        "OTG_FS_HOST": "OTG_FS_GLOBAL",
    }


def test_the_irq_table_keys_merged_and_dropped_irqs_by_their_owner(tmp_path: Path) -> None:
    drop = '[peripherals]\ndrop = ["TIM2"]\nsource = "toy"\n'
    loaded = load_patches([_write(tmp_path, drop + _merge())], part="TOY1", svd_name="toy.svd")
    table = build_irq_table(_device(), None, [], None, owners=peripheral_owners(loaded))
    assert table.by_peripheral == {"OTG_FS_GLOBAL": [67, 68]}


def test_the_irq_table_reconciles_the_irqs_of_a_renamed_peripheral(tmp_path: Path) -> None:
    rename = (
        '[peripherals]\nrenames = { OTG_FS_GLOBAL = "OTG_FS", OTG_FS_HOST = "HOST" }\n'
        'source = "toy"\n'
    )
    loaded = load_patches([_write(tmp_path, rename)], part="TOY1", svd_name="toy.svd")
    irqs = PatchIrqs(drop=["OTG_FS_GLOBAL"], add=[IrqAdd(number=90, name="X", peripheral="HOST")])
    table = build_irq_table(_device(), None, [irqs], None, owners=peripheral_owners(loaded))
    assert table.by_peripheral["HOST"] == [68, 90]
    assert table.by_peripheral["OTG_FS"] == []
    assert all(67 not in numbers for numbers in table.by_peripheral.values())


def test_the_irq_table_without_owners_keys_each_svd_peripheral() -> None:
    table = build_irq_table(_device(), None, [], None)
    assert table.by_peripheral["OTG_FS_HOST"] == [68]
    assert table.by_peripheral["OTG_FS_GLOBAL"] == [67]


@pytest.mark.timeout(120)
def test_a_built_description_keeps_the_irqs_moved_into_an_unrenamed_block(tmp_path: Path) -> None:
    try:
        sources = find_sources("STM32L152RETx")
    except MissingSource as exc:
        pytest.skip(f"STM32L152RETx sources not found: {exc}")
    body = _MERGE.format(into="I2C1", merged='"DAC"', size="0x4000")
    path = _write(tmp_path, body, "zz_merge.toml", '"STM32L152RETx"')
    description = build_description(replace(sources, patches=[*sources.patches, path]))
    by_name = {p.name: p for p in description.peripherals}
    assert "DAC" not in by_name
    assert by_name["I2C1"].irqs == [21, 31, 32]
