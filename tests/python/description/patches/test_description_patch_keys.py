import copy
from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description import _sources
from popcorn.stm32.description._cubemx import DbIp, DbPart
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._patches import (
    PatchReport,
    PatchTarget,
    apply_patches,
    corrected_part,
    load_patches,
)
from popcorn.stm32.description._svd import expand_svd

from ..conftest import target_from_svd, target_peripheral

SVD_DIR = Path(__file__).parents[4] / "tests" / "data" / "stm32" / "svd"

_IPS = {
    "A": DbIp(instance="A", name="TA", version="va"),
    "B": DbIp(instance="B", name="TB", version="vb"),
    "D": DbIp(instance="D", name="TD", version="vd"),
}


def _target(db_ips: dict[str, DbIp] | None = None) -> PatchTarget:
    device = expand_svd((SVD_DIR / "synthetic.svd").read_bytes(), file="synthetic.svd")
    return target_from_svd(device, _IPS if db_ips is None else db_ips)


def _write(tmp_path: Path, text: str, name: str = "patch.toml") -> Path:
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


def _part(ref_name: str, die: str, flash_kib: int) -> DbPart:
    return DbPart(
        ref_name=ref_name,
        family="TOY",
        line="TOY",
        die=die,
        ram_kib=[64],
        flash_kib=[flash_kib],
        ips=[],
    )


def _match_patch(tmp_path: Path, match_lines: str) -> Path:
    return _write(
        tmp_path,
        f"""
format = 1
[match]
parts = ["STM32L15*"]
{match_lines}
""",
    )


_CAT5 = _part("STM32L152RETx", "DIE437", 512)
_CAT6 = _part("STM32L151VDTxX", "DIE437", 384)
_CAT3_427 = _part("STM32L152RCTx", "DIE427", 256)
_CAT3_436 = _part("STM32L152RCTxA", "DIE436", 256)


def _selected(path: Path, part: DbPart) -> bool:
    return bool(load_patches([path], part=part.ref_name, svd_name="x.svd", db_part=part))


def test_dies_and_flash_kib_select_one_category(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, 'dies = ["DIE437"]\nflash_kib = [512]')
    assert _selected(path, _CAT5)
    assert not _selected(path, _CAT6)
    assert not _selected(path, _CAT3_427)
    assert not _selected(path, _CAT3_436)


def test_dies_alone_selects_every_flash_size_of_the_die(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, 'dies = ["DIE437"]')
    assert _selected(path, _CAT5)
    assert _selected(path, _CAT6)
    assert not _selected(path, _CAT3_436)


def test_flash_kib_alone_selects_every_die_of_the_size(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, "flash_kib = [256]")
    assert _selected(path, _CAT3_427)
    assert _selected(path, _CAT3_436)
    assert not _selected(path, _CAT5)


def test_a_dies_list_selects_each_listed_die(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, 'dies = ["DIE427", "DIE436"]\nflash_kib = [256]')
    assert _selected(path, _CAT3_427)
    assert _selected(path, _CAT3_436)
    assert not _selected(path, _CAT5)


def test_the_flash_size_is_the_selected_name_of_a_group(tmp_path: Path) -> None:
    group = DbPart(
        ref_name="STM32L151C(6-8-B)Tx",
        family="TOY",
        line="TOY",
        die="DIE416",
        ram_kib=[10, 10, 16],
        flash_kib=[32, 64, 128],
        ips=[],
    )
    path = _match_patch(tmp_path, "flash_kib = [128]")
    assert _selected(path, group.select("STM32L151CBTx", file="t"))
    assert not _selected(path, group.select("STM32L151C6Tx", file="t"))


def test_a_patch_with_dies_or_flash_kib_needs_the_part(tmp_path: Path) -> None:
    for lines in ('dies = ["DIE437"]', "flash_kib = [512]"):
        path = _match_patch(tmp_path, lines)
        assert load_patches([path], part="STM32L152RETx", svd_name="x.svd") == []


def test_a_part_without_a_flash_element_is_selected_by_no_flash_kib_patch(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, "flash_kib = [512]")
    bare = replace(_part("STM32L152RETx", "DIE437", 512), flash_kib=[])
    assert not _selected(path, bare)


def test_a_patch_without_dies_or_flash_kib_ignores_the_part(tmp_path: Path) -> None:
    path = _match_patch(tmp_path, "")
    assert _selected(path, _CAT5)
    assert load_patches([path], part="STM32L152RETx", svd_name="x.svd")


def _ip_patch(tmp_path: Path, body: str, name: str = "patch.toml") -> Path:
    return _write(
        tmp_path,
        f"""
format = 1
[match]
parts = ["TOY*"]
{body}
""",
        name,
    )


def _apply(target: PatchTarget, *paths: Path) -> PatchReport:
    return apply_patches(load_patches(list(paths), part="TOY1", svd_name="x.svd"), target)


def test_ip_versions_set_the_version_and_the_report_keeps_the_old_one(tmp_path: Path) -> None:
    path = _ip_patch(
        tmp_path,
        """
[ip]
versions = { A = "STM32L1_a_cat46" }
source = "probe"
""",
    )
    target = _target()
    report = _apply(target, path)
    assert target_peripheral(target, "A").ip_version == "STM32L1_a_cat46"
    assert target_peripheral(target, "A").ip == "TA"
    assert target_peripheral(target, "B").ip_version == "vb"
    assert report.ip_versions_set == 1
    assert report.ip_version_changes == [("A", "va", "STM32L1_a_cat46")]


def test_an_ip_versions_glob_sets_every_matching_instance(tmp_path: Path) -> None:
    path = _ip_patch(
        tmp_path,
        """
[ip]
versions = { "[AB]" = "v9" }
source = "probe"
""",
    )
    target = _target()
    report = _apply(target, path)
    assert [target_peripheral(target, n).ip_version for n in ("A", "B", "C", "D")] == [
        "v9",
        "v9",
        "",
        "vd",
    ]
    assert report.ip_versions_set == 2


def test_ip_versions_apply_after_the_ip_renames_of_the_same_patch(tmp_path: Path) -> None:
    path = _ip_patch(
        tmp_path,
        """
[ip]
renames = { B = ["A"] }
versions = { A = "v9" }
source = "probe"
""",
    )
    target = _target()
    _apply(target, path)
    assert target_peripheral(target, "A").ip == "TB"
    assert target_peripheral(target, "A").ip_version == "v9"


@pytest.mark.parametrize(
    ("body", "key", "expected"),
    [
        (
            'versions = { NOPE = "v" }\nsource = "probe"',
            "ip.versions.NOPE",
            "an instance name matching the glob",
        ),
        ('versions = { A = "v" }', "ip.source", "a non-empty source"),
        ('versions = { A = "v" }\nsource = ""', "ip.source", "a non-empty source"),
        (
            'versions = { A = "v", "[AB]" = "w" }\nsource = "probe"',
            "ip.versions.[AB]",
            "instances no other glob of the table sets",
        ),
    ],
)
def test_a_bad_ip_versions_table_is_an_error(
    tmp_path: Path, body: str, key: str, expected: str
) -> None:
    path = _ip_patch(tmp_path, f"[ip]\n{body}")
    target = _target()
    before = copy.deepcopy(target)
    with pytest.raises(DescriptionError) as excinfo:
        _apply(target, path)
    assert excinfo.value.key == key
    assert excinfo.value.expected == expected
    assert target == before


_FAMILY = """
[peripherals]
renames = { A = "AA" }
source = "probe"
"""

_CATEGORY = """
[ip]
versions = { AA = "v9" }
source = "probe"
"""


def test_ip_versions_name_an_instance_renamed_by_an_earlier_file(tmp_path: Path) -> None:
    family = _ip_patch(tmp_path, _FAMILY, "stm32x.toml")
    category = _ip_patch(tmp_path, _CATEGORY, "stm32x_cat1.toml")
    target = _target()
    report = _apply(target, family, category)
    assert target_peripheral(target, "AA").ip_version == "v9"
    assert report.ip_version_changes == [("AA", "va", "v9")]


def test_ip_versions_in_a_file_before_the_rename_still_apply(tmp_path: Path) -> None:
    family = _ip_patch(tmp_path, _FAMILY, "stm32x.toml")
    category = _ip_patch(tmp_path, _CATEGORY, "stm32x_cat1.toml")
    target = _target()
    _apply(target, category, family)
    assert target_peripheral(target, "AA").ip_version == "v9"


def test_a_later_file_ip_rename_keeps_the_version_an_earlier_file_set(tmp_path: Path) -> None:
    versions = _ip_patch(tmp_path, '[ip]\nversions = { A = "v9" }\nsource = "probe"', "stm32x.toml")
    renames = _ip_patch(tmp_path, '[ip]\nrenames_if_present = { B = ["A"] }', "stm32x_cat1.toml")
    target = _target({"B": DbIp(instance="B", name="TB", version="vdb")})
    _apply(target, versions, renames)
    assert target_peripheral(target, "A").ip == "TB"
    assert target_peripheral(target, "A").ip_version == "v9"


def test_the_family_file_sorts_before_its_category_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    for name in (
        "stm32l1_cat4.toml",
        "stm32l1_cat13.toml",
        "stm32l1.toml",
        "stm32l1_cat3_die427.toml",
    ):
        (tmp_path / name).write_text("", encoding="utf-8")
    monkeypatch.setattr(_sources, "_PATCHES_DIR", tmp_path)
    listed = [path.name for path in vars(_sources)["_patch_files"]()]
    assert listed[0] == "stm32l1.toml"
    assert listed == sorted(listed)


_PRESENT = """
[ip]
renames_if_present = { A = ["B", "C", "NOPE"], MISSING = ["D"] }
"""


def test_renames_if_present_applies_what_exists_and_skips_the_rest(tmp_path: Path) -> None:
    path = _ip_patch(tmp_path, _PRESENT)
    target = _target()
    report = _apply(target, path)
    assert (target_peripheral(target, "B").ip, target_peripheral(target, "B").ip_version) == (
        "TA",
        "va",
    )
    assert target_peripheral(target, "C").ip == "TA"
    assert report.ip_renamed == 2
    assert report.ip_renames_skipped == 2


def test_a_skipped_key_leaves_the_instance_untouched(tmp_path: Path) -> None:
    path = _ip_patch(tmp_path, _PRESENT)
    target = _target()
    before = copy.deepcopy(target_peripheral(target, "D"))
    _apply(target, path)
    assert target_peripheral(target, "D") == before


def test_a_skipped_value_removed_by_a_drop_is_not_an_error(tmp_path: Path) -> None:
    path = _ip_patch(
        tmp_path,
        """
[peripherals]
drop = ["B"]
source = "probe"
[ip]
renames_if_present = { A = ["B", "C"] }
""",
    )
    target = _target()
    report = _apply(target, path)
    assert report.ip_renamed == 1
    assert report.ip_renames_skipped == 1
    assert target_peripheral(target, "C").ip == "TA"


@pytest.mark.parametrize(
    ("entry", "key", "expected"),
    [
        ('MISSING = ["A"]', "ip.renames.MISSING", "a db IP instance"),
        ('A = ["NOPE"]', "ip.renames.A", "an SVD peripheral named NOPE"),
    ],
)
def test_ip_renames_stays_strict(tmp_path: Path, entry: str, key: str, expected: str) -> None:
    path = _ip_patch(tmp_path, f"[ip]\nrenames = {{ {entry} }}")
    with pytest.raises(DescriptionError) as excinfo:
        _apply(_target(), path)
    assert excinfo.value.key == key
    assert excinfo.value.expected == expected


def _drop_patch(tmp_path: Path, entry: str, extra: str = "") -> Path:
    return _ip_patch(
        tmp_path,
        f"""
{extra}
[[register_drops]]
{entry}
source = "probe"
""",
    )


_ALIASES = """
[aliases]
prefix = "SEC_"
offset = 0x10000000
"""


def test_register_drops_remove_the_registers_from_the_peripheral_and_its_aliases(
    tmp_path: Path,
) -> None:
    path = _drop_patch(tmp_path, 'peripheral = "D"\nnames = ["CR"]', _ALIASES)
    target = _target()
    report = _apply(target, path)
    assert target_peripheral(target, "D").registers == []
    assert target_peripheral(target, "SEC_D").registers == []
    assert report.registers_dropped == 1


def _with_alternate(target: PatchTarget) -> None:
    peripheral = target_peripheral(target, "A")
    base = next(r for r in peripheral.registers if r.name == "SR2")
    peripheral.registers.append(replace(base, name="SR2_HOST", alternate_of="SR2"))


def test_a_register_drop_refuses_a_register_whose_alternate_stays(tmp_path: Path) -> None:
    target = _target()
    _with_alternate(target)
    before = copy.deepcopy(target)
    with pytest.raises(DescriptionError) as excinfo:
        _apply(target, _drop_patch(tmp_path, 'peripheral = "A"\nnames = ["SR2"]'))
    assert excinfo.value.key == "register_drops[0].names"
    assert excinfo.value.expected == "SR2 dropped with its alternate SR2_HOST"
    assert target == before


def test_a_register_drop_takes_a_register_with_its_alternate(tmp_path: Path) -> None:
    target = _target()
    _with_alternate(target)
    _apply(target, _drop_patch(tmp_path, 'peripheral = "A"\nnames = ["SR2", "SR2_HOST"]'))
    names = {r.name for r in target_peripheral(target, "A").registers}
    assert not {"SR2", "SR2_HOST"} & names
    assert names


def test_register_drops_keep_the_other_registers(tmp_path: Path) -> None:
    path = _drop_patch(tmp_path, 'peripheral = "A"\nnames = ["SR2"]')
    target = _target()
    kept = {r.name for r in target_peripheral(target, "A").registers} - {"SR2"}
    _apply(target, path)
    assert {r.name for r in target_peripheral(target, "A").registers} == kept
    assert kept


@pytest.mark.parametrize(
    ("entry", "key", "expected"),
    [
        (
            'peripheral = "A"\nnames = ["NOPE"]',
            "register_drops[0].names",
            "a register named NOPE in A",
        ),
        ('peripheral = "A"\nnames = []', "register_drops[0].names", "at least one register name"),
        ('peripheral = "A"\nnames = ["SR", "SR"]', "register_drops[0].names", "each name once"),
        (
            'peripheral = "SEC_D"\nnames = ["CR"]',
            "register_drops[0].peripheral",
            "a peripheral of the description that is not an alias",
        ),
        (
            'peripheral = "NOPE"\nnames = ["CR"]',
            "register_drops[0].peripheral",
            "a peripheral of the description that is not an alias",
        ),
    ],
)
def test_a_bad_register_drop_is_an_error(
    tmp_path: Path, entry: str, key: str, expected: str
) -> None:
    path = _drop_patch(tmp_path, entry, _ALIASES)
    target = _target()
    before = copy.deepcopy(target)
    with pytest.raises(DescriptionError) as excinfo:
        _apply(target, path)
    assert excinfo.value.key == key
    assert excinfo.value.expected == expected
    assert target == before


def test_a_rule_naming_a_dropped_register_is_an_error(tmp_path: Path) -> None:
    path = _ip_patch(
        tmp_path,
        """
[[register_drops]]
peripheral = "A"
names = ["SR2"]
source = "probe"
[[resets]]
peripheral = "A"
register = "SR2"
reset = 1
source = "probe"
""",
    )
    with pytest.raises(DescriptionError) as excinfo:
        _apply(_target(), path)
    assert excinfo.value.key == "resets[0].register"


def test_an_empty_ip_table_is_an_error(tmp_path: Path) -> None:
    path = _ip_patch(tmp_path, '[ip]\nsource = "probe"')
    with pytest.raises(DescriptionError) as excinfo:
        _apply(_target(), path)
    assert excinfo.value.key == "ip"
    assert excinfo.value.expected == "a renames, renames_if_present or versions entry"


def _rename_patch(tmp_path: Path, entry: str, extra: str = "") -> Path:
    return _ip_patch(tmp_path, f'{extra}\n[[register_renames]]\n{entry}\nsource = "probe"\n')


def test_register_renames_rename_the_register_of_every_matching_peripheral(tmp_path: Path) -> None:
    target = _target()
    before = [r.offset for r in target_peripheral(target, "A").registers if r.name == "SR2"]
    report = _apply(
        target, _rename_patch(tmp_path, 'peripherals = ["A", "B"]\nrenames = { SR2 = "SRX" }')
    )
    names = {r.name: r.offset for r in target_peripheral(target, "A").registers}
    assert "SR2" not in names
    assert [names["SRX"]] == before
    assert report.registers_renamed == 1 * 2


def test_register_renames_carry_the_alternate_views_of_a_renamed_register(tmp_path: Path) -> None:
    target = _target()
    peripheral = target_peripheral(target, "A")
    base = next(r for r in peripheral.registers if r.name == "SR2")
    other = next(r for r in peripheral.registers if r.name != "SR2")
    peripheral.registers.append(replace(base, name="SR2_HOST", alternate_of="SR2"))
    peripheral.registers.append(replace(other, name="OTHER_HOST", alternate_of=other.name))
    _apply(target, _rename_patch(tmp_path, 'peripherals = ["A"]\nrenames = { SR2 = "SRX" }'))
    renamed = target_peripheral(target, "A").registers
    alternates = {r.name: r.alternate_of for r in renamed if r.alternate_of}
    assert alternates == {"SR2_HOST": "SRX", "OTHER_HOST": other.name}


def test_register_renames_glob_matches_several_peripherals(tmp_path: Path) -> None:
    target = _target()
    _apply(target, _rename_patch(tmp_path, 'peripherals = ["*D"]\nrenames = { CR = "CTL" }'))
    assert "CTL" in {r.name for r in target_peripheral(target, "D").registers}
    assert "CTL" in {r.name for r in target_peripheral(target, "SEC_D").registers}


def test_register_renames_leave_unmatched_peripherals_alone(tmp_path: Path) -> None:
    target = _target()
    _apply(target, _rename_patch(tmp_path, 'peripherals = ["D"]\nrenames = { CR = "CTL" }'))
    assert "CR" in {r.name for r in target_peripheral(target, "SEC_D").registers}


def test_register_renames_reach_the_aliases_of_the_peripheral(tmp_path: Path) -> None:
    target = _target()
    report = _apply(
        target,
        _rename_patch(tmp_path, 'peripherals = ["D"]\nrenames = { CR = "CTL" }', _ALIASES),
    )
    assert "CTL" in {r.name for r in target_peripheral(target, "D").registers}
    assert "CTL" in {r.name for r in target_peripheral(target, "SEC_D").registers}
    assert "CR" not in {r.name for r in target_peripheral(target, "SEC_D").registers}
    assert report.registers_renamed == 1


def test_register_renames_with_a_glob_naming_base_and_alias_rename_both_once(
    tmp_path: Path,
) -> None:
    target = _target()
    report = _apply(
        target,
        _rename_patch(tmp_path, 'peripherals = ["*D"]\nrenames = { CR = "CTL" }', _ALIASES),
    )
    for name in ("D", "SEC_D"):
        assert [r.name for r in target_peripheral(target, name).registers] == ["CTL"]
    assert report.registers_renamed == 1


def test_register_renames_refuse_a_glob_matching_only_aliases(tmp_path: Path) -> None:
    target = _target()
    with pytest.raises(DescriptionError) as excinfo:
        _apply(
            target,
            _rename_patch(tmp_path, 'peripherals = ["SEC_D"]\nrenames = { CR = "CTL" }', _ALIASES),
        )
    assert excinfo.value.key == "register_renames[0].peripherals"
    assert excinfo.value.expected == "a peripheral that is not an alias matching SEC_D"


@pytest.mark.parametrize(
    ("entry", "key", "expected"),
    [
        (
            'peripherals = ["A"]\nrenames = { NOPE = "X" }',
            "register_renames[0].renames.NOPE",
            "a register named NOPE in A",
        ),
        (
            'peripherals = ["A"]\nrenames = { SR2 = "SR" }',
            "register_renames[0].renames.SR2",
            "a name not used in A, not SR",
        ),
        (
            'peripherals = ["ZZ*"]\nrenames = { SR2 = "X" }',
            "register_renames[0].peripherals",
            "a peripheral matching ZZ*",
        ),
        (
            'peripherals = []\nrenames = { SR2 = "X" }',
            "register_renames[0].peripherals",
            "at least one peripheral glob",
        ),
        (
            'peripherals = ["A"]\nrenames = {}',
            "register_renames[0].renames",
            "at least one register to rename",
        ),
    ],
)
def test_register_renames_refuse_a_bad_rule_and_leave_the_target_whole(
    tmp_path: Path, entry: str, key: str, expected: str
) -> None:
    target = _target()
    before = copy.deepcopy(target)
    with pytest.raises(DescriptionError) as excinfo:
        _apply(target, _rename_patch(tmp_path, entry))
    assert (excinfo.value.key, excinfo.value.expected) == (key, expected)
    assert target == before


def test_core_mpu_sets_the_flag_once(tmp_path: Path) -> None:
    target = _target()
    target.core.mpu = False
    path = _ip_patch(tmp_path, '[core]\nmpu = true\nsource = "probe"\n')
    report = _apply(target, path)
    assert target.core.mpu
    assert report.core_fields_set == 1
    second = _ip_patch(tmp_path, '[core]\nmpu = false\nsource = "probe"\n', "second.toml")
    with pytest.raises(DescriptionError) as excinfo:
        _apply(_target(), path, second)
    assert excinfo.value.key == "core.mpu"


_FACTS_PARTS = 'parts = ["STM32L151RCYx", "STM32L100C6UxA"]'
_FACTS = """
format = 1
[match]
parts = ["STM32L151RCYx", "STM32L100C6UxA"]

[[part_facts]]
part = "STM32L151RCYx"
die = "DIE436"
source = "RM0038 30.6.1 note 1"

[[part_facts]]
part = "STM32L100C6UxA"
ram_kib = 4
source = "DS10193 Table 2"
"""


def test_a_part_facts_row_sets_the_die_and_the_sram_of_its_part_only(tmp_path: Path) -> None:
    files = [_write(tmp_path, _FACTS)]
    die = corrected_part(_part("STM32L151RCYx", "DIE427", 256), files)
    assert (die.die, die.ram_kib) == ("DIE436", [64])
    ram = corrected_part(_part("STM32L100C6UxA", "DIE429", 32), files)
    assert (ram.die, ram.ram_kib) == ("DIE429", [4])
    other = _part("STM32L151RCTx", "DIE427", 256)
    assert corrected_part(other, files) is other


def test_part_facts_apply_before_the_die_match(tmp_path: Path) -> None:
    files = [_write(tmp_path, _FACTS, "facts.toml")]
    files.append(_match_patch(tmp_path, 'dies = ["DIE436"]'))
    part = corrected_part(_part("STM32L151RCYx", "DIE427", 256), files)
    loaded = load_patches(files[1:], part="STM32L151RCYx", svd_name="x.svd", db_part=part)
    assert [p.path.name for p in loaded] == ["patch.toml"]


@pytest.mark.parametrize(
    ("old", "new", "key"),
    [
        (
            'parts = ["STM32L151RCYx", "STM32L100C6UxA"]',
            'parts = ["STM32L151RCYx", "STM32L100C6UxA"]\ndies = ["DIE427"]',
            "match",
        ),
        (
            'parts = ["STM32L151RCYx", "STM32L100C6UxA"]',
            'parts = ["STM32L151RCYx"]',
            "part_facts[1].part",
        ),
        ("ram_kib = 4\n", "", "part_facts[1]"),
        ('source = "DS10193 Table 2"', 'source = ""', "part_facts[1].source"),
    ],
)
def test_a_bad_part_facts_patch_is_refused(tmp_path: Path, old: str, new: str, key: str) -> None:
    assert old in _FACTS
    files = [_write(tmp_path, _FACTS.replace(old, new))]
    with pytest.raises(DescriptionError) as excinfo:
        corrected_part(_part("STM32L151RCYx", "DIE427", 256), files)
    assert excinfo.value.key == key


def test_a_second_part_facts_row_for_one_part_is_refused(tmp_path: Path) -> None:
    files = [_write(tmp_path, _FACTS, "a.toml"), _write(tmp_path, _FACTS, "b.toml")]
    with pytest.raises(DescriptionError) as excinfo:
        corrected_part(_part("STM32L151RCYx", "DIE427", 256), files)
    assert excinfo.value.key == "part_facts[0].part"
    assert "a.toml" in str(excinfo.value)
