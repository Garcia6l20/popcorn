from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description import _sources
from popcorn.stm32.description._cubemx import DbPart, expand_refname, read_part
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._model import RegionAccess, RegionDesc, RegionKind
from popcorn.stm32.description._patches import (
    LoadedPatch,
    PatchTarget,
    apply_patches,
    cmsis_suffix,
    load_patches,
)
from popcorn.stm32.description._sources import (
    SearchPaths,
    compute_source_digests,
    find_sources,
    pin_data_sources,
    resolve_part,
)
from popcorn.stm32.description._svd import expand_svd

from ..conftest import require_pin_data, target_from_svd

NAMESPACE = "http://mcd.rou.st.com/modules.php?name=mcu"
GROUP = "STM32T001A(6-8-B)Tx"
SVD = Path(__file__).parents[4] / "tests" / "data" / "stm32" / "svd" / "synthetic.svd"
FACTS = """format = 1
rzone = "STM32_DIE000_16_128.xml"

[core]
fpu_double = false
dsp = false
trustzone = false
mpu_regions = 8
mpu_regions_secure = 0
sau_regions = 0
num_interrupts = 80
source = "probe"
"""


def _part_xml(
    ref: str, *, flash: list[int], ram: list[int], eeprom: list[int], die: str = "DIE000"
) -> bytes:
    rows = "".join(f"<Flash>{kib}</Flash>" for kib in flash)
    rows += "".join(f"<Ram>{kib}</Ram>" for kib in ram)
    rows += "".join(f"<E2prom>{size}</E2prom>" for size in eeprom)
    return (
        f'<Mcu xmlns="{NAMESPACE}" RefName="{ref}" Family="STM32T0" Line="STM32T001">'
        f"<Die>{die}</Die>{rows}</Mcu>"
    ).encode()


def _toy_part(eeprom: list[int]) -> DbPart:
    return read_part(
        _part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16], eeprom=eeprom), file="g"
    )


def test_read_part_gives_the_e2prom_of_the_selected_name() -> None:
    group = _toy_part([2048, 4096, 8192])
    assert group.eeprom == [2048, 4096, 8192]
    sizes = {name: group.select(name, file="g").eeprom_bytes for name, _ in expand_refname(GROUP)}
    assert sizes == {"STM32T001A6Tx": 2048, "STM32T001A8Tx": 4096, "STM32T001ABTx": 8192}


def test_one_e2prom_element_applies_to_every_name_and_none_gives_none() -> None:
    one = _toy_part([4096])
    assert all(one.select(name, file="g").eeprom_bytes == 4096 for name, _ in expand_refname(GROUP))
    none = _toy_part([])
    assert none.eeprom_bytes is None
    assert none.select("STM32T001A8Tx", file="g").eeprom_bytes is None


def test_an_e2prom_count_that_fits_neither_one_nor_the_group_is_refused() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _toy_part([2048, 4096]).select("STM32T001A6Tx", file="g")
    assert excinfo.value.key == "Mcu.E2prom"


def test_the_l1_parts_carry_their_own_e2prom_size() -> None:
    pin_data = require_pin_data()
    stems = sorted(p.stem for p in (pin_data / "mcu").glob("STM32L1*.xml"))
    sizes: dict[str, int | None] = {}
    for stem in stems:
        path = pin_data / "mcu" / f"{stem}.xml"
        for name, _ in expand_refname(stem):
            sizes[name] = read_part(path.read_bytes(), file=str(path), name=name).eeprom_bytes
    assert len(sizes) == 132
    assert all(size is not None and 2048 <= size <= 16384 for size in sizes.values())
    assert sizes["STM32L152RETx"] == 16384
    assert sizes["STM32L151C6Tx"] == 4096
    assert sizes["STM32L100C6Ux"] == 2048
    assert sizes["STM32L152RBTx"] == 4096
    assert resolve_part("STM32L152RETx", pin_data).name == "STM32L152RETx"


def _db(tmp_path: Path, *, rzone: bool, facts: bool) -> tuple[Path, SearchPaths, Path]:
    db_dir = tmp_path / "db"
    (db_dir / "mcu" / "memory").mkdir(parents=True)
    (db_dir / "mcu" / f"{GROUP}.xml").write_bytes(
        _part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16], eeprom=[2048, 4096, 8192])
    )
    if rzone:
        (db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml").write_text("<x/>", encoding="utf-8")
    facts_dir = tmp_path / "facts"
    facts_dir.mkdir()
    if facts:
        (facts_dir / "STM32_DIE000_16_128.toml").write_text(FACTS, encoding="utf-8")
    svd_dir = tmp_path / "svd"
    svd_dir.mkdir()
    (svd_dir / "STM32T001.svd").write_text("<device/>", encoding="utf-8")
    search = SearchPaths(svd_dirs=[svd_dir], cubemx_db=db_dir, cmsis_device_dirs=[])
    return db_dir, search, facts_dir


def test_a_part_without_a_db_rzone_takes_the_packaged_facts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    sources = find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir)
    assert sources.rzone is None
    assert sources.rzone_facts == facts_dir / "STM32_DIE000_16_128.toml"
    digests = compute_source_digests(sources)
    assert "rzone_facts" in digests
    assert "rzone" not in digests


def test_the_db_rzone_stays_the_source_when_both_exist(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=True, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    sources = find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir)
    assert sources.rzone == db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml"
    assert sources.rzone_facts is None
    digests = compute_source_digests(sources)
    assert "rzone" in digests
    assert "rzone_facts" not in digests


def test_a_part_with_neither_rzone_nor_facts_is_a_missing_source_naming_both_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=False)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    with pytest.raises(MissingSource) as excinfo:
        find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir)
    assert "STM32_DIE000_16_128.xml" in excinfo.value.expected
    assert "STM32_DIE000_16_128.toml" in excinfo.value.expected
    assert excinfo.value.path == db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml"


def test_an_rzone_path_that_is_a_directory_is_refused_without_the_facts_fallback(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    (db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml").mkdir()
    with pytest.raises(DescriptionError) as excinfo:
        find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir)
    assert not isinstance(excinfo.value, MissingSource)
    assert "STM32_DIE000_16_128.xml" in excinfo.value.expected


def _patch_dir(tmp_path: Path, *bodies: str) -> Path:
    patch_dir = tmp_path / "patches"
    patch_dir.mkdir()
    for index, body in enumerate(bodies):
        (patch_dir / f"p{index}.toml").write_text(
            f'format = 1\n[match]\nparts = ["STM32T001*"]\n{body}', encoding="utf-8"
        )
    return patch_dir


def _cmsis_dir(tmp_path: Path, *names: str) -> Path:
    include = tmp_path / "Include"
    include.mkdir()
    for name in names:
        (include / name).write_text("// header", encoding="utf-8")
    return include


def _find_with_cmsis(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, patch_bodies: tuple[str, ...], *headers: str
) -> _sources.Sources:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    monkeypatch.setattr(_sources, "_PATCHES_DIR", _patch_dir(tmp_path, *patch_bodies))
    search = replace(search, cmsis_device_dirs=[_cmsis_dir(tmp_path, *headers)])
    return find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir)


_SUFFIX_XE = '[cmsis]\nsuffix = "xe"\nsource = "probe"\n'


def test_a_cmsis_suffix_names_the_header_and_it_is_recorded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sources = _find_with_cmsis(
        tmp_path, monkeypatch, (_SUFFIX_XE,), "stm32t001xx.h", "stm32t001xe.h", "stm32t001xd.h"
    )
    assert sources.cmsis_header is not None
    assert sources.cmsis_header.name == "stm32t001xe.h"
    assert "cmsis_header" in compute_source_digests(sources)


def test_a_part_without_a_cmsis_suffix_keeps_the_xx_header(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sources = _find_with_cmsis(
        tmp_path,
        monkeypatch,
        ('[ip]\nrenames = { }\nsource = "x"\n',),
        "stm32t001xx.h",
        "stm32t001xe.h",
    )
    assert sources.cmsis_header is not None
    assert sources.cmsis_header.name == "stm32t001xx.h"


def test_a_cmsis_suffix_without_its_header_finds_none(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sources = _find_with_cmsis(tmp_path, monkeypatch, (_SUFFIX_XE,), "stm32t001xx.h")
    assert sources.cmsis_header is None


def test_an_explicit_cmsis_header_beats_the_suffix(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    monkeypatch.setattr(_sources, "_PATCHES_DIR", _patch_dir(tmp_path, _SUFFIX_XE))
    explicit = tmp_path / "mine.h"
    explicit.write_text("// header", encoding="utf-8")
    sources = find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir, cmsis_header=explicit)
    assert sources.cmsis_header == explicit


@pytest.mark.parametrize("suffix", ["", "e", "x", "xE", "x e", "xe.h"])
def test_a_malformed_cmsis_suffix_is_refused(tmp_path: Path, suffix: str) -> None:
    patch_dir = _patch_dir(tmp_path, f'[cmsis]\nsuffix = "{suffix}"\nsource = "probe"\n')
    patches = load_patches(sorted(patch_dir.glob("*.toml")), part="STM32T001ABTx", svd_name="")
    with pytest.raises(DescriptionError) as excinfo:
        cmsis_suffix(patches)
    assert excinfo.value.key in {"cmsis.suffix"}


def test_two_patches_setting_the_cmsis_suffix_are_refused(tmp_path: Path) -> None:
    patch_dir = _patch_dir(tmp_path, _SUFFIX_XE, _SUFFIX_XE.replace("xe", "xd"))
    patches = load_patches(sorted(patch_dir.glob("*.toml")), part="STM32T001ABTx", svd_name="")
    with pytest.raises(DescriptionError) as excinfo:
        cmsis_suffix(patches)
    assert excinfo.value.key == "cmsis"


def _eeprom_target(eeprom_bytes: list[int], clash: bool = False) -> PatchTarget:
    target = target_from_svd(expand_svd(SVD.read_bytes(), file="synthetic.svd"))
    target.regions = [
        RegionDesc(
            name="Flash",
            start=0x0800_0000,
            size=0x80000 if clash else 0x40000,
            kind=RegionKind.ROM,
            acc=RegionAccess(read=True, write=False, execute=True),
            secure=False,
        )
    ]
    target.db_part = _toy_part(eeprom_bytes).select("STM32T001ABTx", file="g")
    return target


def _eeprom_patch(tmp_path: Path, entry: str, extra: str = "") -> list[Path]:
    path = tmp_path / "eeprom.toml"
    path.write_text(
        'format = 1\n[match]\nparts = ["STM32T001*"]\n[memory]\nsource = "probe"\n'
        f"{entry}\n{extra}\n",
        encoding="utf-8",
    )
    return [path]


def test_an_eeprom_entry_adds_a_data_eeprom_region_sized_by_the_part(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192])
    patches = load_patches(
        _eeprom_patch(tmp_path, 'eeprom = { start = 0x08080000, controller = "A" }'),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    report = apply_patches(patches, target)
    eeprom = target.regions[-1]
    assert report.regions_added == 1
    assert (eeprom.name, eeprom.start, eeprom.size, eeprom.kind) == (
        "DATA_EEPROM",
        0x0808_0000,
        8192,
        RegionKind.ROM,
    )
    assert eeprom.controller == "A"
    assert not eeprom.acc.write and not eeprom.secure


def test_an_eeprom_entry_without_a_controller_leaves_the_region_bare(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192])
    patches = load_patches(
        _eeprom_patch(tmp_path, "eeprom = { start = 0x08080000 }"),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    apply_patches(patches, target)
    assert target.regions[-1].controller is None


def test_an_eeprom_entry_for_a_part_without_the_element_is_refused(tmp_path: Path) -> None:
    target = _eeprom_target([])
    patches = load_patches(
        _eeprom_patch(tmp_path, "eeprom = { start = 0x08080000 }"),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.eeprom"


def test_an_eeprom_entry_without_a_part_is_refused(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192])
    target.db_part = None
    patches = load_patches(
        _eeprom_patch(tmp_path, "eeprom = { start = 0x08080000 }"),
        part="STM32T001ABTx",
        svd_name="",
    )
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.eeprom"


def test_an_eeprom_region_clashing_with_flash_is_refused(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192], clash=True)
    patches = load_patches(
        _eeprom_patch(tmp_path, "eeprom = { start = 0x08040000 }"),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.eeprom"
    assert excinfo.value.expected == "a range clear of region Flash"


def test_an_eeprom_controller_and_a_controller_entry_for_it_are_refused(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192])
    patches = load_patches(
        _eeprom_patch(
            tmp_path,
            'eeprom = { start = 0x08080000, controller = "A" }',
            'controller = { DATA_EEPROM = "A" }',
        ),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.eeprom"


def test_an_eeprom_controller_must_be_a_peripheral(tmp_path: Path) -> None:
    target = _eeprom_target([2048, 4096, 8192])
    patches = load_patches(
        _eeprom_patch(tmp_path, 'eeprom = { start = 0x08080000, controller = "NOPE" }'),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key == "memory.controller.DATA_EEPROM"


def _alias_patches(tmp_path: Path, target: PatchTarget, entry: str) -> list[LoadedPatch]:
    return load_patches(
        _eeprom_patch(tmp_path, entry),
        part="STM32T001ABTx",
        svd_name="",
        db_part=target.db_part,
    )


def test_an_alias_entry_adds_a_region_sized_and_typed_by_its_target(tmp_path: Path) -> None:
    target = _eeprom_target([2048])
    flash = target.regions[0]
    patches = _alias_patches(
        tmp_path, target, 'alias = [{ name = "BOOT", start = 0x0, region = "Flash" }]'
    )
    report = apply_patches(patches, target)
    boot = target.regions[-1]
    assert report.regions_added == 1
    assert (boot.name, boot.start, boot.size, boot.kind, boot.physical) == (
        "BOOT",
        0,
        flash.size,
        RegionKind.ROM,
        flash.start,
    )
    assert boot.acc == flash.acc and boot.acc is not flash.acc
    assert boot.controller is None and boot.content is None and boot.fill is None
    assert boot.secure == flash.secure


@pytest.mark.parametrize(
    ("entry", "expected"),
    [
        ('{ name = "BOOT", start = 0x0, region = "NOPE" }', "an existing region"),
        (
            '{ name = "BOOT", start = 0x0, region = "BB" }',
            "a region that is neither bit_band nor an alias",
        ),
        (
            '{ name = "BOOT", start = 0x0, region = "ALIAS" }',
            "a region that is neither bit_band nor an alias",
        ),
        (
            '{ name = "BOOT", start = 0x08020000, region = "Flash" }',
            "a range clear of region Flash",
        ),
        (
            '{ name = "BOOT", start = 0x200, region = "Flash" }',
            "a start and size in whole 1 KB pages",
        ),
        ('{ name = "Flash", start = 0x0, region = "Flash" }', "a region name not already used"),
    ],
)
def test_an_alias_entry_is_refused_under_its_key(tmp_path: Path, entry: str, expected: str) -> None:
    target = _eeprom_target([2048])
    flash = target.regions[0]
    target.regions += [
        replace(
            flash,
            name="BB",
            start=0x2200_0000,
            size=0x0200_0000,
            kind=RegionKind.BIT_BAND,
            physical=0x2000_0000,
        ),
        replace(flash, name="ALIAS", start=0x1000_0000, physical=flash.start),
    ]
    patches = _alias_patches(tmp_path, target, f"alias = [{entry}]")
    with pytest.raises(DescriptionError) as excinfo:
        apply_patches(patches, target)
    assert excinfo.value.key in {"memory.alias.BOOT", "memory.alias.Flash"}
    assert excinfo.value.expected == expected


def test_a_malformed_cmsis_suffix_is_refused_beside_an_explicit_header(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    monkeypatch.setattr(
        _sources, "_PATCHES_DIR", _patch_dir(tmp_path, '[cmsis]\nsuffix = "E"\nsource = "probe"\n')
    )
    explicit = tmp_path / "mine.h"
    explicit.write_text("// header", encoding="utf-8")
    with pytest.raises(DescriptionError) as excinfo:
        find_sources("STM32T001ABTx", search=search, cubemx_db=db_dir, cmsis_header=explicit)
    assert excinfo.value.key == "cmsis.suffix"
    assert "or digits" in excinfo.value.expected


def test_pin_data_sources_names_the_cmsis_header_by_the_suffix(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir, search, facts_dir = _db(tmp_path, rzone=False, facts=True)
    part_file = db_dir / "mcu" / f"{GROUP}.xml"
    part_file.write_bytes(
        part_file.read_bytes().replace(
            b"<Die>", b'<IP InstanceName="GPIO" Name="GPIO" Version="v1"/><Die>'
        )
    )
    (db_dir / "mcu" / "IP").mkdir()
    (db_dir / "mcu" / "IP" / "GPIO-v1_Modes.xml").write_text("<x/>", encoding="utf-8")
    monkeypatch.setattr(_sources, "_RZONE_FACTS_DIR", facts_dir)
    monkeypatch.setattr(_sources, "_PATCHES_DIR", _patch_dir(tmp_path, _SUFFIX_XE))
    search = replace(
        search, cmsis_device_dirs=[_cmsis_dir(tmp_path, "stm32t001xx.h", "stm32t001xe.h")]
    )
    sources = pin_data_sources("STM32T001ABTx", pin_data=db_dir, search=search)
    assert sources.cmsis_header is not None
    assert sources.cmsis_header.name == "stm32t001xe.h"
