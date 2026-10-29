from pathlib import Path

import pytest
from popcorn.stm32.description import _patches, _sources
from popcorn.stm32.description._cubemx import DbPart, expand_refname, read_part, rzone_file_name
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._patches import LoadedPatch
from popcorn.stm32.description._sources import (
    ResolvedPart,
    SearchPaths,
    die_ip_sources,
    find_sources,
    resolve_part,
)

from ..conftest import require_pin_data

NAMESPACE = "http://mcd.rou.st.com/modules.php?name=mcu"
GROUP = "STM32T001A(6-8-B)Tx"


def _part_xml(ref: str, *, flash: list[int], ram: list[int], ips: tuple[str, ...] = ()) -> bytes:
    rows = "".join(f"<Flash>{kib}</Flash>" for kib in flash)
    rows += "".join(f"<Ram>{kib}</Ram>" for kib in ram)
    rows += "".join(f'<IP InstanceName="{ip}" Name="{ip}" Version="v1"/>' for ip in ips)
    return (
        f'<Mcu xmlns="{NAMESPACE}" RefName="{ref}" Family="STM32T0" Line="STM32T001">'
        f"<Die>DIE000</Die>{rows}</Mcu>"
    ).encode()


def _group_part() -> DbPart:
    return read_part(_part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16]), file="g.xml")


def test_expand_refname_gives_each_option_in_order() -> None:
    assert expand_refname(GROUP) == [
        ("STM32T001A6Tx", 0),
        ("STM32T001A8Tx", 1),
        ("STM32T001ABTx", 2),
    ]
    assert expand_refname("STM32T001ABCx") == [("STM32T001ABCx", 0)]
    assert expand_refname("STM32T001(CD-EF)x") == [("STM32T001CDx", 0), ("STM32T001EFx", 1)]


@pytest.mark.parametrize("ref", ["A(1-2)B(3-4)", "A(1--2)B", "A()B"])
def test_expand_refname_refuses_two_groups_and_empty_options(ref: str) -> None:
    with pytest.raises(DescriptionError):
        expand_refname(ref)


def test_a_selected_name_gets_its_own_flash_and_ram() -> None:
    group = _group_part()
    assert group.flash_kib == [32, 64, 128]
    chosen = group.select("STM32T001ABTx", file="g.xml")
    assert (chosen.ref_name, chosen.flash_kib, chosen.ram_kib) == ("STM32T001ABTx", [128], [16])
    assert group.select("STM32T001A8Tx", file="g.xml").flash_kib == [64]
    assert rzone_file_name(chosen) == "STM32_DIE000_16_128.xml"


def test_read_part_with_a_name_keeps_its_ips_and_selects() -> None:
    data = _part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16], ips=("USART1", "GPIOA"))
    part = read_part(data, file="g.xml", name="STM32T001A8Tx")
    assert part.ref_name == "STM32T001A8Tx"
    assert part.flash_kib == [64]
    assert [ip.instance for ip in part.ips] == ["USART1", "GPIOA"]


def test_one_ram_element_applies_to_every_name_of_the_group() -> None:
    group = read_part(_part_xml(GROUP, flash=[32, 64, 128], ram=[10]), file="g.xml")
    assert [group.select(n, file="g.xml").ram_kib for n, _ in expand_refname(GROUP)] == [[10]] * 3


def test_a_ram_count_that_fits_neither_one_nor_the_group_is_refused() -> None:
    group = read_part(_part_xml(GROUP, flash=[32, 64, 128], ram=[10, 16]), file="g.xml")
    with pytest.raises(DescriptionError) as excinfo:
        group.select("STM32T001A6Tx", file="g.xml")
    assert excinfo.value.key == "Mcu.Ram"


def test_a_name_outside_the_group_is_refused() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        _group_part().select("STM32T001AZTx", file="g.xml")
    assert "STM32T001ABTx" in excinfo.value.expected


def test_resolve_part_finds_the_group_file_and_index(tmp_path: Path) -> None:
    db_dir = tmp_path / "db"
    (db_dir / "mcu").mkdir(parents=True)
    (db_dir / "mcu" / f"{GROUP}.xml").write_bytes(b"")
    (db_dir / "mcu" / "STM32T002BCTx.xml").write_bytes(b"")
    assert resolve_part("STM32T001ABTx", db_dir) == ResolvedPart(
        name="STM32T001ABTx", ref_name=GROUP, index=2
    )
    assert resolve_part("STM32T001ABT6", db_dir).name == "STM32T001ABTx"
    assert resolve_part("STM32T002BCTx", db_dir).index == 0


def test_the_group_refname_is_no_part_name(tmp_path: Path) -> None:
    db_dir = tmp_path / "db"
    (db_dir / "mcu").mkdir(parents=True)
    (db_dir / "mcu" / f"{GROUP}.xml").write_bytes(b"")
    with pytest.raises(DescriptionError) as excinfo:
        resolve_part(GROUP, db_dir)
    assert "STM32T001A6Tx" in excinfo.value.expected


def test_an_ambiguous_cpn_lists_every_expanded_candidate(tmp_path: Path) -> None:
    db_dir = tmp_path / "db"
    (db_dir / "mcu").mkdir(parents=True)
    (db_dir / "mcu" / "STM32T001A(6-8)Tx.xml").write_bytes(b"")
    (db_dir / "mcu" / "STM32T001AxTx.xml").write_bytes(b"")
    with pytest.raises(DescriptionError) as excinfo:
        resolve_part("STM32T001A6T6", db_dir)
    assert "STM32T001A6Tx" in excinfo.value.expected
    assert "STM32T001AxTx" in excinfo.value.expected


def test_two_files_expanding_to_one_name_are_refused(tmp_path: Path) -> None:
    db_dir = tmp_path / "db"
    (db_dir / "mcu").mkdir(parents=True)
    (db_dir / "mcu" / "STM32T001A(6-8)Tx.xml").write_bytes(b"")
    (db_dir / "mcu" / "STM32T001(A6-A8)Tx.xml").write_bytes(b"")
    with pytest.raises(DescriptionError) as excinfo:
        resolve_part("STM32T001A8Tx", db_dir)
    assert "STM32T001A(6-8)Tx" in excinfo.value.expected
    assert "STM32T001(A6-A8)Tx" in excinfo.value.expected


def _svd_dirs(tmp_path: Path) -> SearchPaths:
    svd_dir = tmp_path / "svd"
    svd_dir.mkdir()
    (svd_dir / "STM32T001.svd").write_text("<device/>", encoding="utf-8")
    return SearchPaths(svd_dirs=[svd_dir], cubemx_db=tmp_path / "db", cmsis_device_dirs=[])


def test_find_sources_of_a_group_name_uses_its_own_facts_and_name(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db_dir = tmp_path / "db"
    (db_dir / "mcu" / "memory").mkdir(parents=True)
    (db_dir / "mcu" / f"{GROUP}.xml").write_bytes(
        _part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16])
    )
    (db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml").write_text("<x/>", encoding="utf-8")
    seen: list[str] = []
    real = _patches.load_patches

    def spy(files: list[Path], part: str, svd_name: str) -> list[LoadedPatch]:
        seen.append(part)
        return real(files, part=part, svd_name=svd_name)

    monkeypatch.setattr(_sources, "load_patches", spy)
    sources = find_sources("STM32T001ABT6", search=_svd_dirs(tmp_path), cubemx_db=db_dir)
    assert sources.part == "STM32T001ABTx"
    assert sources.mcu_db == db_dir / "mcu" / f"{GROUP}.xml"
    assert sources.rzone == db_dir / "mcu" / "memory" / "STM32_DIE000_16_128.xml"
    assert seen
    assert set(seen) == {"STM32T001ABTx"}


def test_a_group_file_is_a_die_donor_through_its_members(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    seen: list[str] = []
    real = _patches.load_patches

    def spy(files: list[Path], part: str, svd_name: str) -> list[LoadedPatch]:
        seen.append(part)
        return real(files, part=part, svd_name=svd_name)

    monkeypatch.setattr(_sources, "load_patches", spy)
    db_dir = tmp_path / "db"
    (db_dir / "mcu").mkdir(parents=True)
    own_path = db_dir / "mcu" / "STM32T001BCTx.xml"
    own_path.write_bytes(_part_xml("STM32T001BCTx", flash=[16], ram=[4], ips=("USART1",)))
    donor_path = db_dir / "mcu" / f"{GROUP}.xml"
    donor_path.write_bytes(
        _part_xml(GROUP, flash=[32, 64, 128], ram=[10, 10, 16], ips=("USART1", "USART2"))
    )
    search = _svd_dirs(tmp_path)
    own = read_part(own_path.read_bytes(), file=str(own_path))
    svd_path = search.svd_dirs[0] / "STM32T001.svd"
    found = die_ip_sources(own, own_path, svd_path, core=None, search=search)
    assert found == {"USART2": donor_path}
    assert seen
    assert not any("(" in part for part in seen)


def test_a_malformed_stem_does_not_hide_the_other_parts(tmp_path: Path) -> None:
    mcu = tmp_path / "mcu"
    mcu.mkdir()
    (mcu / "STM32T001ABCx.xml").write_text("<Mcu/>", encoding="utf-8")
    (mcu / "A(1-2)B(3-4).xml").write_text("<Mcu/>", encoding="utf-8")
    assert resolve_part("STM32T001ABCx", tmp_path).name == "STM32T001ABCx"


def test_the_l1_group_files_expand_to_132_resolvable_names() -> None:
    pin_data = require_pin_data()
    stems = sorted(p.stem for p in (pin_data / "mcu").glob("STM32L1*.xml"))
    assert len(stems) == 90
    expected = {name: (stem, index) for stem in stems for name, index in expand_refname(stem)}
    assert len(expected) == 132
    for name, (stem, index) in expected.items():
        resolved = resolve_part(name, pin_data)
        assert (resolved.name, resolved.ref_name, resolved.index) == (name, stem, index)
    assert resolve_part("STM32L151CBT6", pin_data).name == "STM32L151CBTx"
    rb = resolve_part("STM32L152RBTx", pin_data)
    assert (rb.ref_name, rb.index) == ("STM32L152R(6-8-B)Tx", 2)
    assert resolve_part("STM32L151C6Tx", pin_data).index == 0
    path = pin_data / "mcu" / "STM32L152R(6-8-B)Tx.xml"
    part = read_part(path.read_bytes(), file=str(path), name="STM32L152RBTx")
    assert (part.flash_kib, part.ram_kib) == ([128], [16])
