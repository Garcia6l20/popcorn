"""Every STM32U5 line builds: the per-line patch files select by part and SVD."""

from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description._builder import build_description, build_description_with_report
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import find_sources

pytestmark = pytest.mark.timeout(120)

type _Counts = tuple[int, int, int, int, int, int, int]

COUNTS: dict[str, _Counts] = {
    "STM32U535CBTx": (3, 20, 5, 521, 0, 444, 116),
    "STM32U545CETx": (3, 20, 5, 521, 0, 444, 120),
    "STM32U575QGIx": (10, 26, 0, 584, 1, 476, 121),
    "STM32U585QIIx": (9, 26, 0, 584, 1, 476, 128),
    "STM32U595AIHx": (4, 24, 1, 646, 1, 452, 129),
    "STM32U5A5AJHx": (5, 24, 1, 646, 1, 452, 134),
    "STM32U5F7VJTx": (6, 24, 1, 646, 2, 452, 135),
    "STM32U5G7VJTx": (7, 24, 1, 646, 3, 452, 140),
}
NEW_LINES = tuple(p for p in COUNTS if not p.startswith(("STM32U575", "STM32U585")))
IRQS = {"HSPI1": 131, "OCTOSPI2": 120, "OTFDEC2": 109}
UNOWNED = {
    "STM32U595AIHx": ("HSPI1",),
    "STM32U5A5AJHx": ("HSPI1",),
    "STM32U5F7VJTx": ("HSPI1", "OCTOSPI2"),
    "STM32U5G7VJTx": ("HSPI1", "OCTOSPI2", "OTFDEC2"),
}


@pytest.mark.parametrize("part", COUNTS)
def test_each_u5_line_builds_with_its_patch_counts(part: str) -> None:
    sources = find_sources(part)
    description, report = build_description_with_report(sources)
    files, renamed, skipped, cross, added, fields, irqs = COUNTS[part]
    assert (
        len(sources.patches),
        report.ip_renamed,
        report.ip_renames_skipped,
        report.cross_rules_added,
        report.irqs_added,
        report.fields_touched,
        len(description.irqs),
    ) == (files, renamed, skipped, cross, added, fields, irqs)


@pytest.mark.parametrize("part", COUNTS)
def test_the_otg_fs_core_belongs_to_the_u575_and_u585_only(part: str) -> None:
    names = {p.name for p in build_description_with_report(find_sources(part))[0].peripherals}
    assert ("OTG_FS" in names) == part.startswith(("STM32U575", "STM32U585"))


@pytest.mark.parametrize("part", UNOWNED)
def test_an_irq_of_a_peripheral_the_svd_omits_is_listed_with_no_owner(part: str) -> None:
    description = build_description_with_report(find_sources(part))[0]
    numbers = {irq.number: irq.name for irq in description.irqs}
    owned = {n for p in description.peripherals for n in p.irqs}
    for name in UNOWNED[part]:
        assert numbers[IRQS[name]] == f"{name}_IRQn"
        assert IRQS[name] not in owned


@pytest.mark.parametrize("part", ["STM32U5A5AJHx", "STM32U595AIHx"])
def test_the_gpdma_status_registers_carry_the_trigger_overrun_flag(part: str) -> None:
    description = build_description_with_report(find_sources(part))[0]
    gpdma = next(p for p in description.peripherals if p.name == "GPDMA1")
    for channel in range(16):
        status = next(
            r for r in gpdma.registers if f"GPDMA_C{channel}SR" in (r.name, *(r.aliases or []))
        )
        flag = next(f for f in status.fields if f.name == "TOF")
        assert (flag.lsb, flag.width) == (14, 1)


@pytest.mark.parametrize(
    ("part", "ahb1", "ahb2"),
    [
        ("STM32U535CBTx", 0xD0000100, 0x00000000),
        ("STM32U545CETx", 0xD0000100, 0x00000000),
        ("STM32U575QGIx", 0xD0000100, 0x00000000),
        ("STM32U585QIIx", 0xD0000100, 0x00000000),
        ("STM32U595AIHx", 0xD0200100, 0x80000000),
        ("STM32U5A5AJHx", 0xD0200100, 0x80000000),
        ("STM32U5F7VJTx", 0xD0200100, 0xC0000000),
        ("STM32U5G7VJTx", 0xD0200100, 0xC0000000),
    ],
)
def test_the_rcc_enable_registers_reset_as_the_manual_gives_per_line(
    part: str, ahb1: int, ahb2: int
) -> None:
    description = build_description_with_report(find_sources(part))[0]
    rcc = next(p for p in description.peripherals if p.name == "RCC")
    resets = {n: r.reset for r in rcc.registers for n in (r.name, *(r.aliases or []))}
    assert (resets["RCC_AHB1ENR"], resets["RCC_AHB2ENR2"]) == (ahb1, ahb2)


@pytest.mark.parametrize("part", NEW_LINES)
def test_a_renames_key_no_line_has_stays_an_error(part: str, tmp_path: Path) -> None:
    patch = tmp_path / "zz_wrong_key.toml"
    patch.write_text(
        'format = 1\n[match]\nparts = ["STM32U5*"]\n[ip]\nrenames = { NO_SUCH_IP = ["GPIOA"] }\n'
        'source = "test"\n',
        encoding="utf-8",
    )
    sources = find_sources(part)
    with pytest.raises(DescriptionError) as excinfo:
        build_description(replace(sources, patches=[*sources.patches, patch]))
    assert excinfo.value.key == "ip.renames.NO_SUCH_IP"


UCPD_PAIRS = {
    "FRSEVTCF": "FRSEVT",
    "TYPECEVT2CF": "TYPECEVT2",
    "TYPECEVT1CF": "TYPECEVT1",
    "RXMSGENDCF": "RXMSGEND",
    "RXOVRCF": "RXOVR",
    "RXHRSTDETCF": "RXHRSTDET",
    "RXORDDETCF": "RXORDDET",
    "TXUNDCF": "TXUND",
    "HRSTSENTCF": "HRSTSENT",
    "HRSTDISCCF": "HRSTDISC",
    "TXMSGABTCF": "TXMSGABT",
    "TXMSGSENTCF": "TXMSGSENT",
    "TXMSGDISCCF": "TXMSGDISC",
}
SPI_PAIRS = {
    "SUSPC": "SUSP",
    "MODFC": "MODF",
    "TIFREC": "TIFRE",
    "CRCEC": "CRCE",
    "OVRC": "OVR",
    "UDRC": "UDR",
    "TXTFC": "TXTF",
    "EOTC": "EOT",
}


def _pairs(part: str, peripheral: str, clear_register: str) -> dict[str, str]:
    description = build_description_with_report(find_sources(part))[0]
    found = next(p for p in description.peripherals if p.name == peripheral)
    return {
        r.field: r.target_field
        for r in found.cross_rules
        if clear_register in (r.reg, f"{peripheral[:-1]}_{r.reg}")
    }


@pytest.mark.parametrize(
    "part",
    ["STM32U595AIHx", "STM32U5A5AJHx", "STM32U5F7VJTx", "STM32U5G7VJTx", "STM32U575QGIx"],
)
def test_the_ucpd_clear_flags_pair_with_their_status_flags(part: str) -> None:
    assert _pairs(part, "UCPD1", "ICR" if part != "STM32U575QGIx" else "UCPD_ICR") == UCPD_PAIRS


@pytest.mark.parametrize("part", ["STM32U535CBTx", "STM32U5G7VJTx", "STM32U575QGIx"])
def test_the_spi_clear_flags_pair_with_their_status_flags(part: str) -> None:
    assert _pairs(part, "SPI1", "IFCR" if part != "STM32U575QGIx" else "SPI_IFCR") == SPI_PAIRS


@pytest.mark.parametrize(
    "part", ["STM32U599NJHxQ", "STM32U5A9NJHxQ", "STM32U5F9BJYxQ", "STM32U5G9BJYxQ"]
)
def test_the_u599_and_u5a9_lines_build_with_their_own_svd_irqs(part: str) -> None:
    description = build_description_with_report(find_sources(part))[0]
    owned = {n for p in description.peripherals if p.name != "HSPI1" for n in p.irqs}
    assert any(irq.number == 131 and irq.name == "HSPI1_IRQn" for irq in description.irqs)
    assert 131 not in owned
    gpdma = next(p for p in description.peripherals if p.name == "GPDMA1")
    status = next(r for r in gpdma.registers if "GPDMA_C3SR" in (r.name, *(r.aliases or [])))
    assert any((f.name, f.lsb) == ("TOF", 14) for f in status.fields)
