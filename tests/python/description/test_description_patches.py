import copy
from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import DbIp, DbPart, read_part, read_rzone
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    RegionAccess,
    RegionDesc,
    RegionKind,
    WriteEffect,
)
from popcorn.stm32.description._patches import PatchTarget, apply_patches, load_patches
from popcorn.stm32.description._sources import Sources
from popcorn.stm32.description._svd import expand_svd

from .conftest import target_from_svd, target_peripheral

SVD_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "svd"
DB_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "db"
PATCH_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "patches"
STM32U5_TOML = (
    Path(__file__).parents[3]
    / "python"
    / "popcorn"
    / "stm32"
    / "description"
    / "patches"
    / "stm32u5.toml"
)


def _synthetic_target() -> PatchTarget:
    device = expand_svd((SVD_DIR / "synthetic.svd").read_bytes(), file="synthetic.svd")
    return target_from_svd(device)


def _write_patch(tmp_path: Path, text: str, name: str = "patch.toml") -> Path:
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


def test_load_patches_filters_by_match() -> None:
    patches = load_patches([PATCH_DIR / "good.toml"], part="TOY1", svd_name="synthetic.svd")
    assert len(patches) == 1
    assert patches[0].path == PATCH_DIR / "good.toml"
    assert patches[0].patch.source == "synthetic probe for the patch engine"

    none_match = load_patches([PATCH_DIR / "good.toml"], part="OTHER", svd_name="synthetic.svd")
    assert none_match == []


def test_apply_good_patch_touches_every_rule_kind() -> None:
    device = expand_svd((SVD_DIR / "synthetic.svd").read_bytes(), file="synthetic.svd")
    target = target_from_svd(device, {"IPX": DbIp(instance="IPX", name="TYPEX", version="v1")})
    patches = load_patches([PATCH_DIR / "good.toml"], part="TOY1", svd_name="synthetic.svd")
    report = apply_patches(patches, target)

    assert report.ip_renamed == 1
    assert report.aliased == 1
    assert report.fields_touched == 1 + 3
    assert report.cross_rules_added == 1
    assert report.irqs_dropped == 1
    assert report.irqs_added == 1
    assert report.core_fields_set == 1

    a = target_peripheral(target, "A")
    assert a.ip == "TYPEX"
    sr = next(r for r in a.registers if r.name == "SR")
    f_field = next(f for f in sr.fields if f.name == "F")
    assert f_field.on_write.value == "zero_to_clear"

    sec_d = target_peripheral(target, "SEC_D")
    d = target_peripheral(target, "D")
    assert sec_d.alias_of == "D"
    assert sec_d.ip == d.ip

    b = target_peripheral(target, "B")
    for name in ("ARR0", "ARR1", "ARR2"):
        register = next(r for r in b.registers if r.name == name)
        v_field = next(f for f in register.fields if f.name == "V")
        assert v_field.on_read.value == "set"

    assert len(a.cross_rules) == 1
    cross = a.cross_rules[0]
    assert cross.reg == "SR2"
    assert cross.field == "R"
    assert cross.target_reg == "SR"
    assert cross.target_field == "F"

    assert "A_IRQ" not in {irq.name for irq in target.irqs}
    assert 3 not in a.irqs
    assert any(irq.number == 5 and irq.name == "NEW_IRQ" for irq in target.irqs)
    assert 5 in b.irqs

    assert target.core.reset_clock_hz == 8_000_000


def test_each_range_low_greater_than_high_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    source = "probe"

    [match]
    parts = ["TOY*"]

    [[fields]]
    instances = ["B"]
    registers = ["ARR{n}"]
    fields = ["V"]
    on_read = "set"
    each = { n = [2, 0] }
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0].each.n"


def test_unknown_instance_selector_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["NOPE"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0].instances"


def test_unknown_register_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["A"]
    registers = ["NOPE"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0].registers"


def test_unknown_field_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["NOPE"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0].fields"


def test_unknown_irq_in_drop_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [irqs]
    drop = ["NOPE_IRQ"]
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "irqs.drop.NOPE_IRQ"


def test_add_on_an_existing_irq_number_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [irqs]
    add = [{ number = 3, name = "DUP", peripheral = "B" }]
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "irqs.add.DUP"


def test_selector_with_both_ip_and_instances_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    ip = "A"
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    with pytest.raises(DescriptionError) as excinfo:
        load_patches([path], part="TOY1", svd_name="synthetic.svd")
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0]"


def test_selector_matching_nothing_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    ip = "no_such_ip"
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0]"


def test_two_rules_setting_one_field_differently_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "zero_to_clear"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[1].on_write"


def _width_patch(tmp_path: Path, field: str, width: int, *, twice: bool = False) -> Path:
    rule = f"""
[[fields]]
instances = ["A"]
registers = ["SR"]
fields = ["{field}"]
width = {width}
source = "probe"
"""
    text = """
format = 1
[match]
parts = ["TOY*"]
""" + rule * (2 if twice else 1)
    return _write_patch(tmp_path, text)


def test_a_width_rule_widens_the_field_in_place(tmp_path: Path) -> None:
    target = _synthetic_target()
    path = _width_patch(tmp_path, "R", 3)
    report = apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    sr = next(r for r in target_peripheral(target, "A").registers if r.name == "SR")
    assert [(f.name, f.lsb, f.width) for f in sr.fields] == [("F", 0, 1), ("R", 1, 3)]
    assert next(f for f in sr.fields if f.name == "R").on_read.value == "clear"
    assert report.fields_touched == 1


@pytest.mark.parametrize(
    ("field", "width", "twice", "reason"),
    [
        ("F", 2, False, "bits clear of the other fields"),
        ("R", 32, False, "bits inside the 32-bit register"),
        ("R", 2, True, "a field not already patched"),
    ],
)
def test_a_width_rule_refuses_an_overlap_an_overflow_or_a_second_width(
    tmp_path: Path, field: str, width: int, twice: bool, reason: str
) -> None:
    target = _synthetic_target()
    before = copy.deepcopy(target)
    path = _width_patch(tmp_path, field, width, twice=twice)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == f"fields[{1 if twice else 0}].width"
    assert excinfo.value.expected == reason
    assert target == before


def test_alias_base_mismatch_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [aliases]
    prefix = "SEC_"
    offset = 0x20000000
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "aliases.SEC_D.base"


def test_alias_layout_mismatch_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [aliases]
    prefix = "SEC_"
    offset = 0x10000000
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    sec_d = target_peripheral(target, "SEC_D")
    sec_d.registers = [*list(sec_d.registers), sec_d.registers[0]]

    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "aliases.SEC_D.registers"


def test_missing_source_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    """
    path = _write_patch(tmp_path, text)
    with pytest.raises(DescriptionError) as excinfo:
        load_patches([path], part="TOY1", svd_name="synthetic.svd")
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "fields[0].source"


def test_unknown_key_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    bogus = true
    [match]
    parts = ["TOY*"]
    """
    path = _write_patch(tmp_path, text)
    with pytest.raises(DescriptionError) as excinfo:
        load_patches([path], part="TOY1", svd_name="synthetic.svd")
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "bogus"


def test_wrong_format_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 2
    [match]
    parts = ["TOY*"]
    """
    path = _write_patch(tmp_path, text)
    with pytest.raises(DescriptionError) as excinfo:
        load_patches([path], part="TOY1", svd_name="synthetic.svd")
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "format"


def test_no_partial_application_on_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["NOPE"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    before = copy.deepcopy(target)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError):
        apply_patches(patches, target)
    assert target == before


def test_drop_removes_peripherals_before_every_other_rule(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [peripherals]
    drop = ["C", "B"]
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    names = {p.name for p in target.peripherals}
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    report = apply_patches(patches, target)
    assert report.peripherals_dropped == 2
    assert {p.name for p in target.peripherals} == names - {"B", "C"}


def test_unknown_peripheral_in_drop_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [peripherals]
    drop = ["NOPE"]
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == "peripherals.drop.NOPE"


def test_a_name_dropped_twice_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [peripherals]
    drop = ["A"]
    source = "probe"
    """
    first = _write_patch(tmp_path, text, "family.toml")
    second = _write_patch(tmp_path, text, "part.toml")
    target = _synthetic_target()
    before = copy.deepcopy(target)
    patches = load_patches([first, second], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(second)
    assert excinfo.value.key == "peripherals.drop"
    assert excinfo.value.expected == "each name once"
    assert target == before


def test_a_rule_naming_a_dropped_peripheral_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [peripherals]
    drop = ["A"]
    source = "probe"
    [[fields]]
    instances = ["A"]
    registers = ["SR"]
    fields = ["F"]
    on_write = "modify"
    source = "probe"
    """
    path = _write_patch(tmp_path, text)
    target = _synthetic_target()
    before = copy.deepcopy(target)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "fields[0].instances"
    assert target == before


def test_drop_without_source_is_an_error(tmp_path: Path) -> None:
    text = """
    format = 1
    [match]
    parts = ["TOY*"]
    [peripherals]
    drop = ["A"]
    """
    path = _write_patch(tmp_path, text)
    with pytest.raises(DescriptionError):
        load_patches([path], part="TOY1", svd_name="synthetic.svd")


def test_u575_alias_rule_sets_98_peripherals(
    u575_sources: Sources, st_svd_u575: Path, st_cubemx_db: Path
) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    part = read_part(
        (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes(), file="STM32U575QGIx.xml"
    )
    db_ips = {ip.instance: ip for ip in part.ips}
    target = target_from_svd(device, db_ips)
    target.regions = read_rzone(
        u575_sources.rzone.read_bytes(), file=str(u575_sources.rzone)
    ).regions
    target.db_part = part

    patches = load_patches([STM32U5_TOML], part="STM32U575QGIx", svd_name="STM32U575.svd")
    assert len(patches) == 1
    report = apply_patches(patches, target)

    aliased = [p for p in target.peripherals if p.alias_of is not None]
    assert report.aliased == 98
    assert len(aliased) == 98
    for peripheral in aliased:
        assert peripheral.alias_of is not None
        base_peripheral = target_peripheral(target, peripheral.alias_of)
        assert peripheral.base == base_peripheral.base + 0x10000000

    usart1 = target_peripheral(target, "USART1")
    sec_usart1 = target_peripheral(target, "SEC_USART1")
    assert sec_usart1.ip == usart1.ip == "USART"


_RAM = RegionDesc(
    name="SRAM",
    start=0x2000_0000,
    size=0x1000,
    kind=RegionKind.RAM,
    acc=RegionAccess(read=True, write=True, execute=True),
    secure=False,
)


def _memory_patch(tmp_path: Path, *entries: str) -> Path:
    lines = ",\n".join(f"  {entry}" for entry in entries)
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[memory]
source = "probe"
add = [
{lines}
]
"""
    return _write_patch(tmp_path, text)


_OTP = (
    '{ name = "OTP", start = 0x0BFA0000, size = 0x400, kind = "rom", '
    "acc = { read = true, write = false, execute = false }, secure = false }"
)
_OTP_ALIAS = (
    '{ name = "OTP_S", start = 0x0FFA0000, size = 0x400, kind = "rom", '
    "acc = { read = true, write = false, execute = false }, secure = true, physical = 0x0BFA0000 }"
)


def test_memory_add_appends_regions_after_the_existing_ones(tmp_path: Path) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    patches = load_patches(
        [_memory_patch(tmp_path, _OTP, _OTP_ALIAS)], part="TOY1", svd_name="synthetic.svd"
    )
    report = apply_patches(patches, target)
    assert report.regions_added == 2
    assert [r.name for r in target.regions] == ["SRAM", "OTP", "OTP_S"]
    otp, alias = target.regions[1], target.regions[2]
    assert (otp.start, otp.size, otp.kind, otp.secure, otp.physical) == (
        0x0BFA_0000,
        0x400,
        RegionKind.ROM,
        False,
        None,
    )
    assert otp.acc == RegionAccess(read=True, write=False, execute=False)
    assert (alias.secure, alias.physical) == (True, 0x0BFA_0000)


@pytest.mark.parametrize(
    ("entry", "reason"),
    [
        (
            '{ name = "SRAM", start = 0x10000000, size = 0x400, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a region name not already used",
        ),
        (
            '{ name = "LOW", start = 0x1FFFFC00, size = 0x800, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a range clear of region SRAM",
        ),
        (
            '{ name = "HIGH", start = 0x20000C00, size = 0x800, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a range clear of region SRAM",
        ),
        (
            '{ name = "EMPTY", start = 0x30000000, size = 0, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a non-empty range inside the 32-bit space",
        ),
        (
            '{ name = "WRAP", start = 0xFFFFFC00, size = 0x800, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a non-empty range inside the 32-bit space",
        ),
        (
            '{ name = "HALF", start = 0x30000000, size = 0x200, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a start and size in whole 1 KB pages",
        ),
        (
            '{ name = "SKEW", start = 0x30000200, size = 0x400, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = false }",
            "a start and size in whole 1 KB pages",
        ),
        (
            '{ name = "NOWHERE", start = 0x30000000, size = 0x400, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = true, "
            "physical = 0x40000000 }",
            "a physical range inside an existing region",
        ),
        (
            '{ name = "PAST", start = 0x30000000, size = 0x800, kind = "ram", '
            "acc = { read = true, write = true, execute = false }, secure = true, "
            "physical = 0x20000C00 }",
            "a physical range inside an existing region",
        ),
    ],
)
def test_memory_add_refuses_a_clash_an_overlap_or_a_bad_range(
    tmp_path: Path, entry: str, reason: str
) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    before = copy.deepcopy(target)
    path = _memory_patch(tmp_path, _OTP, entry)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    name = entry.split('"')[1]
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == f"memory.add.{name}"
    assert excinfo.value.expected == reason
    assert target == before


def test_memory_add_refuses_two_added_regions_that_overlap(tmp_path: Path) -> None:
    target = _synthetic_target()
    path = _memory_patch(tmp_path, _OTP, _OTP.replace('"OTP"', '"OTP2"'))
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.add.OTP2"


def _controller_patch(tmp_path: Path, controller: str, *entries: str) -> Path:
    lines = ",\n".join(f"  {entry}" for entry in entries)
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[memory]
source = "probe"
controller = {{ {controller} }}
add = [
{lines}
]
"""
    return _write_patch(tmp_path, text)


def test_memory_controller_names_the_peripheral_serving_stores(tmp_path: Path) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    path = _controller_patch(tmp_path, 'OTP = "A", OTP_S = "C"', _OTP, _OTP_ALIAS)
    report = apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert report.controllers_set == 2
    assert [(r.name, r.controller) for r in target.regions] == [
        ("SRAM", None),
        ("OTP", "A"),
        ("OTP_S", "C"),
    ]


def test_memory_controller_alone_needs_no_added_region(tmp_path: Path) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    target.regions[0].acc = RegionAccess(read=True, write=False, execute=True)
    text = """
format = 1
[match]
parts = ["TOY*"]
[memory]
source = "probe"
controller = { SRAM = "D" }
"""
    report = apply_patches(
        load_patches([_write_patch(tmp_path, text)], part="TOY1", svd_name="synthetic.svd"),
        target,
    )
    assert report.controllers_set == 1
    assert report.regions_added == 0
    assert target.regions[0].controller == "D"


@pytest.mark.parametrize(
    ("controller", "region", "reason"),
    [
        ('NOWHERE = "A"', "NOWHERE", "an existing region"),
        ('SRAM = "A"', "SRAM", "a read-only region"),
        ('OTP = "NOPE"', "OTP", "a peripheral of the description that is not an alias"),
    ],
)
def test_memory_controller_refuses_a_bad_region_or_peripheral(
    tmp_path: Path, controller: str, region: str, reason: str
) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    before = copy.deepcopy(target)
    path = _controller_patch(tmp_path, controller, _OTP)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == f"memory.controller.{region}"
    assert excinfo.value.expected == reason
    assert target == before


def test_memory_controller_refuses_a_second_controller(tmp_path: Path) -> None:
    target = _synthetic_target()
    first = _controller_patch(tmp_path, 'OTP = "A"', _OTP)
    second = tmp_path / "second.toml"
    second.write_text(
        """
format = 1
[match]
parts = ["TOY*"]
[memory]
source = "probe"
controller = { OTP = "C" }
""",
        encoding="utf-8",
    )
    patches = load_patches([first, second], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.controller.OTP"
    assert excinfo.value.expected == "a region without a controller"


def test_memory_controller_refuses_a_dropped_peripheral(tmp_path: Path) -> None:
    target = _synthetic_target()
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[peripherals]
source = "probe"
drop = ["C"]
[memory]
source = "probe"
controller = {{ OTP = "C" }}
add = [{_OTP}]
"""
    patches = load_patches([_write_patch(tmp_path, text)], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.controller.OTP"
    assert excinfo.value.expected == "a peripheral of the description that is not an alias"


def test_memory_without_add_or_controller_is_refused(tmp_path: Path) -> None:
    text = """
format = 1
[match]
parts = ["TOY*"]
[memory]
source = "probe"
"""
    path = _write_patch(tmp_path, text)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, _synthetic_target())
    assert excinfo.value.key == "memory"
    assert excinfo.value.expected == "an add or a controller entry"


def test_memory_controller_refuses_an_alias(tmp_path: Path) -> None:
    target = _synthetic_target()
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[aliases]
prefix = "SEC_"
offset = 0x10000000
[memory]
source = "probe"
controller = {{ OTP = "SEC_D" }}
add = [{_OTP}]
"""
    patches = load_patches([_write_patch(tmp_path, text)], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.controller.OTP"
    assert excinfo.value.expected == "a peripheral of the description that is not an alias"


_NEW_FIELDS = '[{ name = "LOW", lsb = 0, width = 3 }, { name = "HIGH", lsb = 3, width = 3 }]'


def _register_patch(tmp_path: Path, entry: str, *, extra: str = "") -> Path:
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[aliases]
prefix = "SEC_"
offset = 0x10000000
{extra}
[[registers]]
{entry}
source = "probe"
"""
    return _write_patch(tmp_path, text)


def _register_entry(**overrides: str) -> str:
    keys = {
        "peripheral": '"D"',
        "name": '"NSCR"',
        "offset": "0x08",
        "width": "32",
        "reset": "0x3F",
        "acc": '"read_write"',
        "fields": _NEW_FIELDS,
    } | overrides
    return "\n".join(f"{key} = {value}" for key, value in keys.items())


def test_registers_add_lands_in_the_peripheral_and_its_aliases(tmp_path: Path) -> None:
    target = _synthetic_target()
    fields_rule = """
[[fields]]
instances = ["D"]
registers = ["NSCR"]
fields = ["LOW"]
on_write = "one_to_clear"
source = "probe"
"""
    path = _register_patch(tmp_path, _register_entry(offset="0x04"), extra=fields_rule)
    report = apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert report.registers_added == 1
    for name in ("D", "SEC_D"):
        peripheral = target_peripheral(target, name)
        assert [(r.name, r.offset) for r in peripheral.registers] == [("CR", 0), ("NSCR", 4)]
    added = target_peripheral(target, "D").registers[1]
    assert (added.width, added.reset, added.reset_mask, added.acc) == (
        32,
        0x3F,
        0xFFFF_FFFF,
        Access.READ_WRITE,
    )
    assert [(f.name, f.lsb, f.width, f.acc) for f in added.fields] == [
        ("HIGH", 3, 3, Access.READ_WRITE),
        ("LOW", 0, 3, Access.READ_WRITE),
    ]
    assert added.fields[1].on_write == WriteEffect.ONE_TO_CLEAR
    assert target_peripheral(target, "SEC_D").registers[1].fields[1].on_write == (
        WriteEffect.MODIFY
    )


@pytest.mark.parametrize(
    ("overrides", "key", "reason"),
    [
        (
            {"peripheral": '"Z"'},
            "peripheral",
            "a peripheral of the description that is not an alias",
        ),
        (
            {"peripheral": '"SEC_D"'},
            "peripheral",
            "a peripheral of the description that is not an alias",
        ),
        ({"width": "24"}, "width", "8, 16 or 32"),
        ({"offset": "0x06"}, "offset", "an offset aligned to 4 inside 0x400"),
        ({"offset": "0x400"}, "offset", "an offset aligned to 4 inside 0x400"),
        ({"width": "16", "offset": "0x3FE", "reset": "0x10000"}, "reset", "a value of 16 bits"),
        ({"name": '"CR"'}, "name", "a name not used in D"),
        ({"offset": "0x00"}, "offset", "a range clear of CR"),
        ({"width": "8", "offset": "0x03", "reset": "0"}, "offset", "a range clear of CR"),
        (
            {"fields": '[{ name = "TOP", lsb = 30, width = 3 }]'},
            "fields.TOP",
            "bits inside the 32-bit register",
        ),
        (
            {"fields": '[{ name = "A", lsb = 0, width = 3 }, { name = "B", lsb = 2, width = 2 }]'},
            "fields.B",
            "bits clear of the other fields",
        ),
        (
            {"fields": '[{ name = "A", lsb = 0, width = 1 }, { name = "A", lsb = 4, width = 1 }]'},
            "fields.A",
            "each name once",
        ),
    ],
)
def test_registers_add_refuses_a_bad_register(
    tmp_path: Path, overrides: dict[str, str], key: str, reason: str
) -> None:
    target = _synthetic_target()
    before = copy.deepcopy(target)
    path = _register_patch(tmp_path, _register_entry(**overrides))
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.key == f"registers[0].{key}"
    assert excinfo.value.expected == reason
    assert target == before


def _size_patch(tmp_path: Path, size: str, peripheral: str = '"D"') -> Path:
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[aliases]
prefix = "SEC_"
offset = 0x10000000

[[peripheral_sizes]]
peripheral = {peripheral}
size = {size}
source = "probe"
"""
    return _write_patch(tmp_path, text)


def test_peripheral_sizes_grow_the_peripheral_and_its_aliases(tmp_path: Path) -> None:
    target = _synthetic_target()
    path = _size_patch(tmp_path, "0x2000")
    report = apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert report.sizes_set == 1
    assert [target_peripheral(target, n).size for n in ("D", "SEC_D")] == [0x2000, 0x2000]


@pytest.mark.parametrize(
    ("size", "peripheral", "reason"),
    [
        ("0x2000", '"Z"', "a peripheral of the description that is not an alias"),
        ("0x2000", '"SEC_D"', "a peripheral of the description that is not an alias"),
        ("0x100", '"D"', "a size of at least 0x400"),
    ],
)
def test_peripheral_sizes_refuse_a_bad_rule(
    tmp_path: Path, size: str, peripheral: str, reason: str
) -> None:
    target = _synthetic_target()
    before = copy.deepcopy(target)
    path = _size_patch(tmp_path, size, peripheral)
    patches = load_patches([path], part="TOY1", svd_name="synthetic.svd")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.file == str(path)
    assert excinfo.value.expected == reason
    assert target == before


def test_registers_add_refuses_two_adds_at_one_offset(tmp_path: Path) -> None:
    target = _synthetic_target()
    second = f"""
[[registers]]
{_register_entry(name='"NSCR2"')}
source = "probe"
"""
    path = _register_patch(tmp_path, _register_entry(), extra=second)
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert excinfo.value.key == "registers[1].offset"
    assert excinfo.value.expected == "a range clear of NSCR2"


def test_registers_add_needs_a_source(tmp_path: Path) -> None:
    text = f"""
format = 1
[match]
parts = ["TOY*"]
[[registers]]
{_register_entry()}
"""
    with pytest.raises(DescriptionError) as excinfo:
        load_patches([_write_patch(tmp_path, text)], part="TOY1", svd_name="synthetic.svd")
    assert excinfo.value.key == "registers[0].source"
    assert excinfo.value.expected == "required"


_SIG_REGIONS = f"""
[memory]
source = "probe"
add = [{_OTP}, {_OTP_ALIAS}]
"""


def _signature_patch(tmp_path: Path, name: str = "patch.toml", **overrides: str) -> Path:
    keys = {
        "region": '"OTP"',
        "package": "0x100",
        "uid": "0x300",
        "flash_size": "0x3A0",
        "uid_words": "[0x11, 0x22, 0x33]",
        "packages": '{ "LQFP64" = 0x00, "LQFP64 SMPS" = 0x08, "LQFP100 DSI SMPS" = 0x14 }',
        "source": '"probe"',
    } | overrides
    body = "\n".join(f"{key} = {value}" for key, value in keys.items())
    text = f"""
format = 1
[match]
parts = ["TOY*"]
{_SIG_REGIONS if name == "patch.toml" else ""}
[signature]
{body}
"""
    return _write_patch(tmp_path, text, name)


def _part(ref_name: str = "TOY1", package: str = "LQFP64", *ips: str) -> DbPart:
    return DbPart(
        ref_name=ref_name,
        family="TOY",
        line="TOY",
        die="DIE1",
        ram_kib=[64],
        flash_kib=[1024],
        ips=[DbIp(instance=ip, name=ip, version="") for ip in ips],
        package=package,
    )


def _signed(tmp_path: Path, part: DbPart | None, *paths: Path) -> PatchTarget:
    target = _synthetic_target()
    target.db_part = part
    files = list(paths) or [_signature_patch(tmp_path)]
    apply_patches(load_patches(files, part="TOY1", svd_name="synthetic.svd"), target)
    return target


def _content(target: PatchTarget, region: str) -> list[tuple[int, bytes]]:
    found = next(r for r in target.regions if r.name == region)
    return [(piece.offset, bytes(piece.data)) for piece in found.content or []]


def test_signature_writes_package_uid_and_flash_size_into_its_region(tmp_path: Path) -> None:
    target = _synthetic_target()
    target.db_part = _part()
    patches = load_patches([_signature_patch(tmp_path)], part="TOY1", svd_name="synthetic.svd")
    report = apply_patches(patches, target)
    assert report.signature_words == 3
    assert _content(target, "OTP") == [
        (0x100, bytes.fromhex("0000")),
        (0x300, bytes.fromhex("110000002200000033000000")),
        (0x3A0, (1024).to_bytes(2, "little")),
    ]
    assert _content(target, "OTP_S") == []


def test_signature_content_entries_land_in_its_region_beside_the_words(
    tmp_path: Path,
) -> None:
    target = _synthetic_target()
    target.db_part = _part()
    path = _signature_patch(
        tmp_path, content="[{ offset = 0x3A5, data = [0xEB, 0x19] }, { offset = 0x10, data = [7] }]"
    )
    report = apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert report.signature_words == 5
    assert _content(target, "OTP") == [
        (0x10, b"\x07"),
        (0x100, bytes.fromhex("0000")),
        (0x300, bytes.fromhex("110000002200000033000000")),
        (0x3A0, (1024).to_bytes(2, "little")),
        (0x3A5, b"\xeb\x19"),
    ]


@pytest.mark.parametrize(
    ("part", "code"),
    [
        (_part("TOY1Q"), b"\x08\x00"),
        (_part("TOY1Q", "LQFP100", "DSIHOST"), b"\x14\x00"),
        (_part("TOY1", "LQFP100", "DSIHOST"), None),
        (_part("TOY1", "UFQFPN48"), None),
    ],
)
def test_signature_package_code_follows_package_dsi_and_smps(
    tmp_path: Path, part: DbPart, code: bytes | None
) -> None:
    pieces = dict(_content(_signed(tmp_path, part), "OTP"))
    assert pieces.get(0x100) == code
    assert pieces[0x3A0] == (1024).to_bytes(2, "little")


@pytest.mark.parametrize(
    ("overrides", "key", "reason"),
    [
        (
            {"region": '"NOPE"'},
            "signature.region",
            "a read-only region of the description, not an alias",
        ),
        (
            {"region": '"OTP_S"'},
            "signature.region",
            "a read-only region of the description, not an alias",
        ),
        (
            {"region": '"SRAM"'},
            "signature.region",
            "a read-only region of the description, not an alias",
        ),
        ({"uid_words": "[1, 2]"}, "signature.uid_words", "three 32-bit words"),
        (
            {"packages": '{ "LQFP64" = 0x20 }'},
            "signature.packages.LQFP64",
            "a PKG code, 0 to 0x1f",
        ),
        (
            {"packages": '{ "LQFP64" = -1 }'},
            "signature.packages.LQFP64",
            "a PKG code, 0 to 0x1f",
        ),
        ({"uid_words": "[1, 2, 0x100000000]"}, "signature.uid_words", "three 32-bit words"),
        (
            {"flash_size": "0x3FF"},
            "signature.flash_size",
            "an offset leaving 2 bytes inside OTP",
        ),
        ({"uid": "0x3F8"}, "signature.uid", "an offset leaving 12 bytes inside OTP"),
        ({"uid": "0x0F8"}, "signature.package", "bytes clear of the content at 0xf8"),
        ({"flash_size": "0x30A"}, "signature.uid", "bytes clear of the content at 0x30a"),
        (
            {"content": "[{ offset = 0x200, data = [] }]"},
            "signature.content[0].data",
            "bytes, at least one",
        ),
        (
            {"content": "[{ offset = 0x200, data = [1] }, { offset = 0x210, data = [0x100] }]"},
            "signature.content[1].data",
            "bytes, at least one",
        ),
        (
            {"content": "[{ offset = 0x3FF, data = [1, 2] }]"},
            "signature.content[0]",
            "an offset leaving 2 bytes inside OTP",
        ),
        (
            {"content": "[{ offset = 0x305, data = [1] }]"},
            "signature.content[0]",
            "bytes clear of the content at 0x300",
        ),
    ],
)
def test_signature_refuses_a_bad_region_word_or_layout(
    tmp_path: Path, overrides: dict[str, str], key: str, reason: str
) -> None:
    target = _synthetic_target()
    target.regions = [copy.deepcopy(_RAM)]
    target.db_part = _part()
    before = copy.deepcopy(target)
    path = _signature_patch(tmp_path, **overrides)
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(load_patches([path], part="TOY1", svd_name="synthetic.svd"), target)
    assert (excinfo.value.file, excinfo.value.key, excinfo.value.expected) == (
        str(path),
        key,
        reason,
    )
    assert target == before


def test_signature_needs_the_cubemx_part(tmp_path: Path) -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _signed(tmp_path, None)
    assert (excinfo.value.key, excinfo.value.expected) == (
        "signature",
        "the CubeMX part of the description",
    )


def test_a_second_signature_over_the_same_bytes_is_refused(tmp_path: Path) -> None:
    first = _signature_patch(tmp_path)
    second = _signature_patch(tmp_path, "second.toml")
    with pytest.raises(DescriptionError) as excinfo:
        _signed(tmp_path, _part(), first, second)
    assert (excinfo.value.file, excinfo.value.key) == (str(second), "signature.flash_size")
