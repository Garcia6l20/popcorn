import copy
from collections.abc import Mapping
from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import DbIp, read_part
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import CoreDesc, IrqDesc, PeripheralDesc
from popcorn.stm32.description._patches import PatchTarget, apply_patches, load_patches
from popcorn.stm32.description._svd import SvdDevice, expand_svd

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


def _target(device: SvdDevice, db_ips: Mapping[str, DbIp] | None = None) -> PatchTarget:
    db_ips = db_ips or {}
    peripherals: list[PeripheralDesc] = []
    irqs: dict[int, IrqDesc] = {}
    for sp in device.peripherals:
        matched = db_ips.get(sp.name)
        ip = matched.name if matched is not None else (sp.group or sp.name)
        ip_version = matched.version if matched is not None else ""
        peripherals.append(
            PeripheralDesc(
                name=sp.name,
                ip=ip,
                ip_version=ip_version,
                base=sp.base,
                size=sp.size,
                registers=sp.registers,
                cross_rules=[],
                irqs=[number for number, _ in sp.interrupts],
            )
        )
        for number, name in sp.interrupts:
            irqs.setdefault(number, IrqDesc(number=number, name=name))
    core = CoreDesc(
        cpu=device.cpu.name,
        revision=device.cpu.revision,
        fpu=device.cpu.fpu_present,
        fpu_double=device.cpu.fpu_dp,
        mpu=device.cpu.mpu_present,
        mpu_regions=0,
        mpu_regions_secure=0,
        sau_regions=device.cpu.sau_num_regions,
        trustzone=False,
        dsp=device.cpu.dsp_present,
        nvic_prio_bits=device.cpu.nvic_prio_bits,
        num_irqs=len(irqs),
        vendor_systick_config=device.cpu.vendor_systick_config,
        reset_clock_hz=0,
    )
    return PatchTarget(
        peripherals=list(peripherals), irqs=list(irqs.values()), core=core, db_ips=db_ips
    )


def _synthetic_target() -> PatchTarget:
    device = expand_svd((SVD_DIR / "synthetic.svd").read_bytes(), file="synthetic.svd")
    return _target(device)


def _peripheral(target: PatchTarget, name: str) -> PeripheralDesc:
    for p in target.peripherals:
        if p.name == name:
            return p
    raise KeyError(name)


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
    target = _target(device, {"IPX": DbIp(instance="IPX", name="TYPEX", version="v1")})
    patches = load_patches([PATCH_DIR / "good.toml"], part="TOY1", svd_name="synthetic.svd")
    report = apply_patches(patches, target)

    assert report.ip_renamed == 1
    assert report.aliased == 1
    assert report.fields_touched == 1 + 3
    assert report.cross_rules_added == 1
    assert report.irqs_dropped == 1
    assert report.irqs_added == 1
    assert report.core_fields_set == 1

    a = _peripheral(target, "A")
    assert a.ip == "TYPEX"
    sr = next(r for r in a.registers if r.name == "SR")
    f_field = next(f for f in sr.fields if f.name == "F")
    assert f_field.on_write.value == "zero_to_clear"

    sec_d = _peripheral(target, "SEC_D")
    d = _peripheral(target, "D")
    assert sec_d.alias_of == "D"
    assert sec_d.ip == d.ip

    b = _peripheral(target, "B")
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
    sec_d = _peripheral(target, "SEC_D")
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


def test_u575_alias_rule_sets_98_peripherals(st_svd_u575: Path, st_cubemx_db: Path) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    part = read_part(
        (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes(), file="STM32U575QGIx.xml"
    )
    db_ips = {ip.instance: ip for ip in part.ips}
    target = _target(device, db_ips)

    patches = load_patches([STM32U5_TOML], part="STM32U575QGIx", svd_name="STM32U575.svd")
    assert len(patches) == 1
    report = apply_patches(patches, target)

    aliased = [p for p in target.peripherals if p.alias_of is not None]
    assert report.aliased == 98
    assert len(aliased) == 98
    for peripheral in aliased:
        assert peripheral.alias_of is not None
        base_peripheral = _peripheral(target, peripheral.alias_of)
        assert peripheral.base == base_peripheral.base + 0x10000000

    usart1 = _peripheral(target, "USART1")
    sec_usart1 = _peripheral(target, "SEC_USART1")
    assert sec_usart1.ip == usart1.ip == "USART"
