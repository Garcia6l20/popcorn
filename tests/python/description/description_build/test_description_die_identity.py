"""Instances a package drops take the IP identity of the die reference, the part of the same die
and SVD with the most bonded pins, then the largest flash, then the first RefName."""

from __future__ import annotations

import functools
import json
import os
import shutil
from dataclasses import replace
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import OptionBytes
from popcorn.stm32 import support
from popcorn.stm32.description import _sources
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._cubemx import DbPart, read_part
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._layers import BLOCK_KEYS, block_key
from popcorn.stm32.description._shipped import build_text
from popcorn.stm32.description._sources import (
    SearchPaths,
    Sources,
    compute_source_digests,
    die_ip_sources,
    find_sources,
    pin_data_sources,
)

PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"
PREBUILT = Path(__file__).parents[3] / "firmware" / "periph" / "prebuilt"
U575_DROPPED = ["DCMI", "FMC", "PSSI", "SAI2", "SDMMC1", "SDMMC2", "UART5", "VREFBUF"]
H562_DROPPED = ["FMC", "SPI4", "SPI5", "UART12", "UART8", "UART9", "USART10"]


def _pin_data() -> Path:
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    return Path(value)


def _peripherals(text: str) -> dict[str, dict[str, Any]]:
    data = cast(dict[str, Any], json.loads(text))
    return {p["name"]: p for p in cast(list[dict[str, Any]], data["peripherals"])}


@functools.cache
def _built(part: str) -> str:
    return build_text(part, pin_data=_pin_data())


@pytest.mark.parametrize(
    ("part", "reference", "dropped"),
    [
        ("STM32U575CGTx", "STM32U575AIIx", U575_DROPPED),
        ("STM32H562RGTx", "STM32H562IIKx", H562_DROPPED),
    ],
)
def test_the_dropped_instances_map_to_the_die_reference(
    part: str, reference: str, dropped: list[str]
) -> None:
    sources = pin_data_sources(part, pin_data=_pin_data())
    assert sorted(sources.die_ip) == dropped
    assert {path.stem for path in sources.die_ip.values()} == {reference}
    digests = compute_source_digests(sources)
    assert sorted(key for key in digests if key.startswith("die_ip:")) == [
        f"die_ip:{instance}" for instance in dropped
    ]


@pytest.mark.parametrize("part", ["STM32U575QGIx", "STM32H573AIIx", "STM32U575AIIx"])
def test_a_part_listing_every_instance_of_its_reference_has_no_donor(part: str) -> None:
    assert pin_data_sources(part, pin_data=_pin_data()).die_ip == {}


@pytest.mark.parametrize(
    ("part", "reference", "instances"),
    [
        (
            "STM32U575CGTx",
            "STM32U575AIIx",
            {"UART5": ("UART", "sci2_v3_0_Cube"), "SAI2": ("SAI", "sai1_L5_Cube")},
        ),
        (
            "STM32H562RGTx",
            "STM32H562IIKx",
            {
                "SPI4": ("SPI", "spi2s3_v2_1_Cube"),
                "UART8": ("UART", "sci2_v3_0_Cube"),
                "USART10": ("USART", "sci3_v2_0_Cube"),
            },
        ),
    ],
)
def test_a_dropped_instance_has_the_reference_identity_and_block(
    part: str,
    reference: str,
    instances: dict[str, tuple[str, str]],
) -> None:
    small, ref = _peripherals(_built(part)), _peripherals(_built(reference))
    assert {name: (small[name]["ip"], small[name]["ip_version"]) for name in instances} == (
        instances
    )
    assert [p["cross_rules"] != [] for p in (small[name] for name in instances)] == [True] * len(
        instances
    )
    differing = [
        name
        for name, peripheral in small.items()
        if block_key({key: peripheral[key] for key in BLOCK_KEYS})
        != block_key({key: ref[name][key] for key in BLOCK_KEYS})
    ]
    assert differing == []


def _description(part: str) -> popcorn.PartDescription:
    return popcorn.parse_description(_built(part), part)


@pytest.mark.parametrize("tzen", [False, True])
@pytest.mark.parametrize(
    ("part", "firmware", "registers"),
    [
        ("STM32U575CGTx", "rcc_ll.elf", {"UART5": 0x1C}),
        ("STM32H562RGTx", "h573/rcc_ll.elf", {"SPI4": 0x14, "UART8": 0x1C, "USART10": 0x1C}),
    ],
)
def test_a_dropped_instance_gets_its_builtin_model(
    part: str, firmware: str, registers: dict[str, int], tzen: bool
) -> None:
    """support() names the built-in model, and a host read of the instance under
    strict_peripherals is served by it."""
    description = _description(part)
    models = {s.name: s.model for s in support(description, trustzone=tzen)}
    assert {name: models[name] for name in registers} == dict.fromkeys(registers, "builtin")
    machine = popcorn.Machine(
        description,
        image=popcorn.Image.elf(PREBUILT / firmware),
        option_bytes=OptionBytes(tzen=tzen),
        strict_peripherals=True,
    )
    machine.reset()
    bases = {p.name: p.base for p in description.peripherals}
    for name, offset in registers.items():
        _ = machine.mem.u32[bases[name] + offset]


def _part_file(directory: Path, name: str, die: str, flash: int, pins: int, ips: list[str]) -> None:
    pin_lines = "".join(f'<Pin Name="P{chr(65 + n // 16)}{n % 16}"/>' for n in range(pins))
    ip_lines = "".join(
        f'<IP InstanceName="{ip}" Name="{ip.rstrip("0123456789")}" Version="{name}_v"/>'
        for ip in ips
    )
    (directory / f"{name}.xml").write_text(
        '<Mcu xmlns="http://mcd.rou.st.com/modules.php?name=mcu" '
        f'RefName="{name}" Family="STM32T0" Line="STM32T0">'
        f"<Die>{die}</Die><Ram>64</Ram><Flash>{flash}</Flash>{ip_lines}{pin_lines}</Mcu>",
        encoding="utf-8",
    )


@pytest.fixture
def fake_db(tmp_path: Path) -> tuple[Path, SearchPaths]:
    """A db `mcu/` directory and SVD files for the fake STM32T001 and STM32T0015 lines."""
    mcu = tmp_path / "db" / "mcu"
    mcu.mkdir(parents=True)
    svd_dir = tmp_path / "svd"
    svd_dir.mkdir()
    for stem in ("STM32T001", "STM32T0015"):
        (svd_dir / f"{stem}.svd").write_text("<device/>", encoding="utf-8")
    search = SearchPaths(svd_dirs=[svd_dir], cubemx_db=tmp_path / "db", cmsis_device_dirs=[])
    return mcu, search


def _donors(mcu: Path, search: SearchPaths, part: str) -> dict[str, str]:
    path = mcu / f"{part}.xml"
    db_part = read_part(path.read_bytes(), file=str(path))
    svd = search.svd_dirs[0] / "STM32T001.svd"
    found = die_ip_sources(db_part, path, svd, core=None, search=search)
    return {instance: donor.stem for instance, donor in found.items()}


def test_the_donor_has_the_most_bonded_pins_then_flash_then_name(
    fake_db: tuple[Path, SearchPaths],
) -> None:
    mcu, search = fake_db
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    _part_file(mcu, "STM32T001Ax", "DIE1", 64, 9, ["UART1", "UART2", "SPI1"])
    _part_file(mcu, "STM32T001Bx", "DIE1", 128, 8, ["UART1", "UART3"])
    _part_file(mcu, "STM32T001Dx", "DIE1", 64, 9, ["UART1", "UART4"])
    assert _donors(mcu, search, "STM32T001Cx") == {"UART2": "STM32T001Ax", "SPI1": "STM32T001Ax"}
    assert _donors(mcu, search, "STM32T001Ax") == {}
    assert _donors(mcu, search, "STM32T001Dx") == {"UART2": "STM32T001Ax", "SPI1": "STM32T001Ax"}
    _part_file(mcu, "STM32T001Dx", "DIE1", 128, 9, ["UART1", "UART4"])
    assert _donors(mcu, search, "STM32T001Ax") == {"UART4": "STM32T001Dx"}


def test_a_part_of_another_die_or_svd_is_no_donor(fake_db: tuple[Path, SearchPaths]) -> None:
    mcu, search = fake_db
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    _part_file(mcu, "STM32T001Ax", "DIE1", 64, 9, ["UART1", "UART2"])
    _part_file(mcu, "STM32T001Zx", "DIE2", 64, 20, ["UART1", "PKA"])
    _part_file(mcu, "STM32T0015Ax", "DIE1", 64, 20, ["UART1", "AES"])
    assert _donors(mcu, search, "STM32T001Cx") == {"UART2": "STM32T001Ax"}


def test_a_part_file_the_db_reader_refuses_is_no_candidate(
    fake_db: tuple[Path, SearchPaths],
) -> None:
    mcu, search = fake_db
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    (mcu / "STM32T001Ax.xml").write_text("<Mcu", encoding="utf-8")
    assert _donors(mcu, search, "STM32T001Cx") == {}


def _cubemx_db() -> Path:
    try:
        sources = find_sources("STM32U575CGTx")
    except DescriptionError as exc:
        pytest.skip(f"STM32U575CGTx sources not found: {exc}")
    return sources.mcu_db.parent.parent


@pytest.fixture
def db_copy(tmp_path: Path) -> Path:
    """The U575 part files, the STM32U575CGTx rzone and its GPIO modes file, copied from the
    CubeMX db into a scratch db."""
    db = _cubemx_db()
    sources = find_sources("STM32U575CGTx")
    copy = tmp_path / "db"
    (copy / "mcu" / "memory").mkdir(parents=True)
    (copy / "mcu" / "IP").mkdir()
    for path in (db / "mcu").glob("STM32U575*.xml"):
        shutil.copy(path, copy / "mcu" / path.name)
    assert sources.rzone is not None and sources.gpio_modes is not None
    shutil.copy(sources.rzone, copy / "mcu" / "memory" / sources.rzone.name)
    shutil.copy(sources.gpio_modes, copy / "mcu" / "IP" / sources.gpio_modes.name)
    return copy


@pytest.mark.timeout(60)
def test_an_edit_of_the_donor_file_rebuilds_the_cached_description(
    db_copy: Path, tmp_path: Path
) -> None:
    cache = tmp_path / "cache"
    first = cached_description("STM32U575CGTx", cubemx_db=db_copy, cache_dir=cache)
    assert cached_description("STM32U575CGTx", cubemx_db=db_copy, cache_dir=cache) == first
    donor = db_copy / "mcu" / "STM32U575AIIx.xml"
    text = donor.read_text(encoding="utf-8")
    edited = text.replace(
        'InstanceName="UART5" Name="UART" Version="sci2_v3_0_Cube"',
        'InstanceName="UART5" Name="UART" Version="sci2_v3_0_Edit"',
    )
    assert edited != text
    donor.write_text(edited, encoding="utf-8")
    second = cached_description("STM32U575CGTx", cubemx_db=db_copy, cache_dir=cache)
    assert second != first
    versions = [
        _peripherals(path.read_text(encoding="utf-8"))["UART5"]["ip_version"]
        for path in (first, second)
    ]
    assert versions == ["sci2_v3_0_Cube", "sci2_v3_0_Edit"]


def test_a_donor_file_without_the_instance_is_refused_naming_it(tmp_path: Path) -> None:
    sources: Sources = pin_data_sources("STM32U575CGTx", pin_data=_pin_data())
    donor = sources.die_ip["UART5"]
    with pytest.raises(DescriptionError) as caught:
        build_description(replace(sources, die_ip={"NOSUCH1": donor}))
    assert (caught.value.file, caught.value.key) == (str(donor), "Mcu.IP.NOSUCH1")


@pytest.mark.timeout(60)
def test_a_donated_dsihost_is_no_package_evidence_for_the_signature(tmp_path: Path) -> None:
    sources: Sources = pin_data_sources("STM32U575CGTx", pin_data=_pin_data())
    donor = sources.die_ip["UART5"]
    text = donor.read_text(encoding="utf-8")
    marker = '<IP InstanceName="UART5"'
    assert marker in text
    copy = tmp_path / donor.name
    copy.write_text(
        text.replace(
            marker, '<IP InstanceName="DSIHOST" Name="DSIHOST" Version="dsi_v1"/>' + marker
        ),
        encoding="utf-8",
    )
    description = build_description(replace(sources, die_ip={**sources.die_ip, "DSIHOST": copy}))
    page = next(r for r in description.regions if r.name == "SIG_NS")
    words = {piece.offset: bytes(piece.data) for piece in page.content or []}
    assert words[0x100] == bytes([0x05, 0])


def _count_reads(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    reads: list[str] = []
    real = read_part

    def counting(data: bytes, *, file: str) -> DbPart:
        reads.append(Path(file).stem)
        return real(data, file=file)

    monkeypatch.setattr(_sources, "read_part", counting)
    return reads


def test_a_repeated_lookup_reads_no_candidate_file_until_one_changes(
    fake_db: tuple[Path, SearchPaths], monkeypatch: pytest.MonkeyPatch
) -> None:
    mcu, search = fake_db
    _sources.die_ip_memo.clear()
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    _part_file(mcu, "STM32T001Ax", "DIE1", 64, 9, ["UART1", "UART2"])
    first = _donors(mcu, search, "STM32T001Cx")
    reads = _count_reads(monkeypatch)
    assert _donors(mcu, search, "STM32T001Cx") == first == {"UART2": "STM32T001Ax"}
    assert reads == []
    reads.clear()
    candidate = mcu / "STM32T001Ax.xml"
    info = candidate.stat()
    os.utime(candidate, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000))
    assert _donors(mcu, search, "STM32T001Cx") == first
    assert reads == ["STM32T001Ax"]
    reads.clear()
    assert _donors(mcu, search, "STM32T001Cx") == first
    assert reads == []
    own = mcu / "STM32T001Cx.xml"
    info = own.stat()
    os.utime(own, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000))
    assert _donors(mcu, search, "STM32T001Cx") == first
    assert reads == ["STM32T001Ax"]


def test_the_memo_holds_at_most_64_lookups(fake_db: tuple[Path, SearchPaths]) -> None:
    mcu, search = fake_db
    _sources.die_ip_memo.clear()
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    for index in range(100):
        _donors(
            mcu, replace(search, svd_dirs=[search.svd_dirs[0], mcu / f"d{index}"]), "STM32T001Cx"
        )
    assert len(_sources.die_ip_memo) == 64


def test_an_unreadable_sibling_part_file_is_no_candidate(
    fake_db: tuple[Path, SearchPaths], monkeypatch: pytest.MonkeyPatch
) -> None:
    mcu, search = fake_db
    _sources.die_ip_memo.clear()
    _part_file(mcu, "STM32T001Cx", "DIE1", 64, 4, ["UART1"])
    _part_file(mcu, "STM32T001Ax", "DIE1", 64, 9, ["UART1", "UART2"])
    _part_file(mcu, "STM32T001Bx", "DIE1", 64, 8, ["UART1", "UART3"])
    real = Path.read_bytes

    def refusing(path: Path) -> bytes:
        if path.stem == "STM32T001Ax":
            raise PermissionError(path)
        return real(path)

    monkeypatch.setattr(Path, "read_bytes", refusing)
    assert _donors(mcu, search, "STM32T001Cx") == {"UART3": "STM32T001Bx"}
