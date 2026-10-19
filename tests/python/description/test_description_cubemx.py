import time
from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import (
    MAX_GPIO_MODES_BYTES,
    DbIp,
    DbPart,
    Rzone,
    gpio_modes_file_name,
    instance_ips,
    read_gpio_alternates,
    read_part,
    read_rzone,
    read_rzone_facts,
    rzone_file_name,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import AlternateDesc, RegionAccess, RegionDesc, RegionKind
from popcorn.stm32.description._svd import expand_svd

DATA_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "db"


def _region(rzone: Rzone, name: str):
    for region in rzone.regions:
        if region.name == name:
            return region
    raise KeyError(name)


def test_synthetic_part_die_ram_flash_and_ips() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    assert part.ref_name == "STM32T001ABCx"
    assert part.family == "STM32T0"
    assert part.line == "STM32T001"
    assert part.die == "DIE000"
    assert part.ram_kib == [16, 4]
    assert part.flash_kib == [64]
    assert {ip.instance for ip in part.ips} == {"USART1", "USB_OTG_FS", "GPIOA"}


def test_synthetic_rzone_file_name_uses_first_ram_and_flash() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    assert rzone_file_name(part) == "STM32_DIE000_16_64.xml"


def test_namespace_less_part_is_a_description_error() -> None:
    data = b"""<?xml version="1.0"?>
    <Mcu RefName="X"><Die>DIE000</Die><Ram>16</Ram><Flash>64</Flash></Mcu>
    """
    with pytest.raises(DescriptionError) as excinfo:
        read_part(data, file="nons.xml")
    assert excinfo.value.key == "Mcu"


CUBEMX_NS = b"http://mcd.rou.st.com/modules.php?name=mcu"
OPEN_PIN_DATA_NS = b"http://dummy.com"
OTHER_NAMESPACES = [
    b"http://example.com/mcu",
    b"http://dummy.com/",
    b"http://dummy.com?name=mcu2",
    b"http://mcd.rou.st.com/modules.php?name=mcu2",
]


def _renamespaced(data: bytes, namespace: bytes) -> bytes:
    assert data.count(CUBEMX_NS) == 1
    return data.replace(CUBEMX_NS, namespace)


def test_an_open_pin_data_part_reads_as_its_cubemx_copy() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    opened = _renamespaced(data, OPEN_PIN_DATA_NS)
    assert read_part(opened, file="open.xml") == read_part(data, file="STM32T001ABCx.xml")


@pytest.mark.parametrize("namespace", OTHER_NAMESPACES, ids=lambda ns: ns.decode())
def test_a_part_of_another_namespace_is_a_description_error(namespace: bytes) -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    with pytest.raises(DescriptionError) as excinfo:
        read_part(_renamespaced(data, namespace), file="other.xml")
    assert excinfo.value.key == "Mcu"
    assert "http://dummy.com" in str(excinfo.value)


def test_malformed_part_xml_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_part(b"<Mcu><Die>DIE000</Die>", file="broken.xml")
    assert excinfo.value.key == "<root>"


def test_synthetic_rzone_processor_and_regions() -> None:
    data = (DATA_DIR / "mcu" / "memory" / "STM32_DIE000_16_64.xml").read_bytes()
    rzone = read_rzone(data, file="STM32_DIE000_16_64.xml")
    assert rzone.processor.clock_hz == 80_000_000
    assert rzone.processor.core == "ARM Cortex-M0"
    assert rzone.processor.core_version == "r0p0"
    assert rzone.processor.fpu is None
    assert rzone.processor.dsp is False
    assert rzone.processor.mpu is True
    assert rzone.processor.tz is False
    assert rzone.processor.num_interrupts == 32
    assert rzone.processor.mpu_regions == 8
    assert rzone.processor.sec_mpu_regions == 0
    assert rzone.processor.sau_regions == 0
    assert len(rzone.regions) == 4

    flash = _region(rzone, "Flash_NS")
    assert flash.start == 0x08000000
    assert flash.size == 0x00010000
    assert flash.kind == RegionKind.ROM
    assert flash.acc.read is True
    assert flash.acc.write is False
    assert flash.acc.execute is True
    assert flash.secure is False
    assert flash.physical is None

    alias = _region(rzone, "SRAM_ALIAS")
    assert alias.physical == 0x20000000
    assert alias.secure is False
    assert alias.acc.read is True
    assert alias.acc.write is True
    assert alias.acc.execute is True

    secure = _region(rzone, "SRAM_S")
    assert secure.secure is True
    assert secure.physical is None

    rw_only = _region(rzone, "BKPSRAM_NS")
    assert rw_only.acc.read is True
    assert rw_only.acc.write is True
    assert rw_only.acc.execute is False
    assert rw_only.physical is None
    assert rw_only.secure is False


def test_unsupported_memory_type_is_a_description_error() -> None:
    data = (DATA_DIR / "mcu" / "memory" / "STM32_DIE000_bad.xml").read_bytes()
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone(data, file="STM32_DIE000_bad.xml")
    assert excinfo.value.key == "memory[OTP_AREA].type"


def test_malformed_rzone_xml_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone(b"<rzone><resources>", file="broken.xml")
    assert excinfo.value.key == "<root>"


def test_instance_ips_exact_match_then_rename() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    result = instance_ips(
        part,
        svd_names={"USART1", "OTG_FS", "GPIOA", "UNKNOWN"},
        renames={"USB_OTG_FS": ["OTG_FS"]},
    )
    assert result.keys() == {"USART1", "OTG_FS", "GPIOA"}
    assert result["USART1"].instance == "USART1"
    assert result["USART1"].name == "USART"
    assert result["OTG_FS"].instance == "USB_OTG_FS"
    assert result["OTG_FS"].name == "USB_OTG"
    assert "UNKNOWN" not in result


@pytest.fixture(scope="session")
def u575_part(st_cubemx_db: Path) -> DbPart:
    data = (st_cubemx_db / "mcu" / "STM32U575QGIx.xml").read_bytes()
    return read_part(data, file="STM32U575QGIx.xml")


@pytest.fixture(scope="session")
def u575_rzone(st_cubemx_db: Path, u575_part: DbPart) -> Rzone:
    file_name = rzone_file_name(u575_part)
    data = (st_cubemx_db / "mcu" / "memory" / file_name).read_bytes()
    return read_rzone(data, file=file_name)


def test_u575_part_and_rzone_file_name(u575_part: DbPart) -> None:
    assert u575_part.ref_name == "STM32U575QGIx"
    assert u575_part.die == "DIE482"
    assert u575_part.ram_kib == [786]
    assert u575_part.flash_kib == [1024]
    assert rzone_file_name(u575_part) == "STM32_DIE482_786_1024.xml"
    assert len(u575_part.ips) == 110


def test_u575_processor(u575_rzone: Rzone) -> None:
    processor = u575_rzone.processor
    assert processor.clock_hz == 160_000_000
    assert processor.num_interrupts == 125
    assert processor.sau_regions == 8
    assert processor.fpu == "SP_FPU"
    assert processor.tz is True
    assert processor.mpu_regions == 16
    assert processor.sec_mpu_regions == 8


def test_u575_ten_regions_match_i05(u575_rzone: Rzone) -> None:
    expected = {
        "Flash_NS": (0x08000000, 0x00100000, "rx", False, None, RegionKind.ROM),
        "RAM_C_NS": (0x0A000000, 0x000C0000, "rwx", False, 0x20000000, RegionKind.RAM),
        "Flash_S": (0x0C000000, 0x00100000, "rx", True, 0x08000000, RegionKind.ROM),
        "RAM_C_S": (0x0E000000, 0x000C0000, "rwx", True, 0x20000000, RegionKind.RAM),
        "SRAM_NS": (0x20000000, 0x000C0000, "rwx", False, None, RegionKind.RAM),
        "SRAM_SRD_NS": (0x28000000, 0x00004000, "rwx", False, None, RegionKind.RAM),
        "SRAM_S": (0x30000000, 0x000C0000, "rwx", True, 0x20000000, RegionKind.RAM),
        "SRAM_SRD_S": (0x38000000, 0x00004000, "rwx", True, 0x28000000, RegionKind.RAM),
        "BKPSRAM_NS": (0x40036400, 0x00000800, "rw", False, None, RegionKind.RAM),
        "BKPSRAM_S": (0x50036400, 0x00000800, "rw", True, 0x40036400, RegionKind.RAM),
    }
    assert len(u575_rzone.regions) == 10
    assert {r.name for r in u575_rzone.regions} == expected.keys()
    for region in u575_rzone.regions:
        start, size, access, secure, physical, kind = expected[region.name]
        assert region.start == start
        assert region.size == size
        assert region.acc.read == ("r" in access)
        assert region.acc.write == ("w" in access)
        assert region.acc.execute == ("x" in access)
        assert region.secure == secure
        assert region.physical == physical
        assert region.kind == kind


_U575_RENAMES = {
    "USB_OTG_FS": ["OTG_FS"],
    "DCACHE1": ["DCACHE"],
    "ICACHE": ["ICache"],
    "COMP1": ["COMP"],
    "COMP2": ["COMP"],
    "OPAMP1": ["OPAMP"],
    "OPAMP2": ["OPAMP"],
    "GPIO": [
        "GPIOA",
        "GPIOB",
        "GPIOC",
        "GPIOD",
        "GPIOE",
        "GPIOF",
        "GPIOG",
        "GPIOH",
        "GPIOI",
        "LPGPIO1",
    ],
    "GTZC": ["GTZC1_TZSC", "GTZC1_TZIC"],
    "GTZC_NS": ["GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3"],
    "GTZC_S": ["GTZC2_TZSC", "GTZC2_TZIC", "GTZC2_MPCBB4"],
    "SYS": ["SYSCFG"],
}


def test_u575_ip_join_unmatched_peripherals_match_i05(st_svd_u575: Path, u575_part: DbPart) -> None:
    device = expand_svd(st_svd_u575.read_bytes(), file=str(st_svd_u575))
    svd_names = {p.name for p in device.peripherals}
    result = instance_ips(u575_part, svd_names, _U575_RENAMES)
    unmatched = svd_names - result.keys()
    non_sec_unmatched = {name for name in unmatched if not name.startswith("SEC_")}
    sec_unmatched = {name for name in unmatched if name.startswith("SEC_")}

    assert non_sec_unmatched == {
        "ADC12",
        "CRS",
        "DBGMCU",
        "DCB",
        "DLYBOS1",
        "DLYBOS2",
        "DLYBSD1",
        "DLYBSD2",
        "EXTI",
        "FDCAN1_RAM",
        "OTFDEC1",
        "OTFDEC2",
        "PKA",
    }
    assert sec_unmatched == {name for name in svd_names if name.startswith("SEC_")}
    assert result["USART1"].name == "USART"
    assert result["OTG_FS"].instance == "USB_OTG_FS"


_MODES_HEAD = b"""<?xml version="1.0"?>
<IP xmlns="http://mcd.rou.st.com/modules.php?name=mcu" Name="GPIO" Version="v1_synth">
"""


def _signal(name: str, value: str) -> bytes:
    return (
        f'<PinSignal Name="{name}"><SpecificParameter Name="GPIO_AF">'
        f"<PossibleValue>{value}</PossibleValue></SpecificParameter></PinSignal>"
    ).encode()


def _modes(*pins: tuple[str, list[bytes]]) -> bytes:
    body = b"".join(
        f'<GPIO_Pin PortName="P{name[1]}" Name="{name}">'.encode()
        + b"".join(signals)
        + b"</GPIO_Pin>"
        for name, signals in pins
    )
    return _MODES_HEAD + body + b"</IP>"


def test_gpio_alternates_keep_package_pins_and_signals_sorted_lower_case() -> None:
    data = _modes(
        (
            "PA6",
            [
                _signal("TIM3_CH1", "GPIO_AF2_TIM3"),
                _signal("SPI1_MISO", "GPIO_AF5_SPI1"),
                _signal("TIM1_BKIN", "GPIO_AF1_TIM1"),
                _signal("NOT_ON_PACKAGE", "GPIO_AF7_X"),
            ],
        ),
        ("PA13 (JTMS/SWDIO)", [_signal("TIM9_ETR", "GPIO_AF0_TIM9")]),
        ("PB2", [_signal("TIM4_CH2", "GPIO_AF2_TIM4"), _signal("EVENTOUT", "no AF")]),
        ("PC3", [_signal("TIM5_CH4", "GPIO_AF2_TIM5")]),
        ("PD16", [_signal("TIM5_CH1", "GPIO_AF2_TIM5")]),
    )
    pins = {
        "PA6": frozenset({"TIM3_CH1", "SPI1_MISO", "TIM1_BKIN"}),
        "PA13 (JTMS/SWDIO)": frozenset({"TIM9_ETR"}),
        "PB2": frozenset({"TIM4_CH2", "EVENTOUT"}),
        "PD16": frozenset({"TIM5_CH1"}),
    }
    alternates = read_gpio_alternates(data, file="modes.xml", pins=pins)
    assert alternates == {
        "A": [
            AlternateDesc(pin=6, af=1, signal="tim1_bkin"),
            AlternateDesc(pin=6, af=2, signal="tim3_ch1"),
            AlternateDesc(pin=6, af=5, signal="spi1_miso"),
            AlternateDesc(pin=13, af=0, signal="tim9_etr"),
        ],
        "B": [AlternateDesc(pin=2, af=2, signal="tim4_ch2")],
    }


def test_open_pin_data_gpio_alternates_read_as_their_cubemx_copy() -> None:
    data = _modes(
        ("PA6", [_signal("TIM3_CH1", "GPIO_AF2_TIM3"), _signal("SPI1_MISO", "GPIO_AF5_SPI1")]),
        ("PB2", [_signal("TIM4_CH2", "GPIO_AF2_TIM4")]),
    )
    pins = {"PA6": frozenset({"TIM3_CH1", "SPI1_MISO"}), "PB2": frozenset({"TIM4_CH2"})}
    cubemx = read_gpio_alternates(data, file="modes.xml", pins=pins)
    opened = read_gpio_alternates(_renamespaced(data, OPEN_PIN_DATA_NS), file="open.xml", pins=pins)
    assert opened == cubemx
    assert sum(len(entries) for entries in cubemx.values()) == 3


@pytest.mark.parametrize("namespace", OTHER_NAMESPACES, ids=lambda ns: ns.decode())
def test_gpio_alternates_of_another_namespace_are_a_description_error(namespace: bytes) -> None:
    data = _modes(("PA6", [_signal("TIM3_CH1", "GPIO_AF2_TIM3")]))
    with pytest.raises(DescriptionError) as excinfo:
        read_gpio_alternates(
            _renamespaced(data, namespace), file="other.xml", pins={"PA6": frozenset({"TIM3_CH1"})}
        )
    assert excinfo.value.key == "IP"


def test_gpio_alternates_outside_the_namespace_are_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_gpio_alternates(b"<IP/>", file="modes.xml", pins={})
    assert excinfo.value.key == "IP"


def test_a_part_root_given_as_the_modes_file_is_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_gpio_alternates(b'<Mcu xmlns="http://dummy.com"/>', file="modes.xml", pins={})
    assert excinfo.value.key == "IP"


def test_malformed_gpio_alternates_are_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_gpio_alternates(b"<IP", file="modes.xml", pins={})
    assert excinfo.value.key == "<root>"


def test_gpio_modes_file_name_follows_the_gpio_ip_version() -> None:
    data = (DATA_DIR / "mcu" / "STM32T001ABCx.xml").read_bytes()
    part = read_part(data, file="STM32T001ABCx.xml")
    assert gpio_modes_file_name(part) == "GPIO-v1_synth_Modes.xml"
    bare = DbPart(
        ref_name="X",
        family="",
        line="",
        die="D",
        ram_kib=[1],
        flash_kib=[1],
        ips=[DbIp(instance="GPIOA", name="GPIO", version="")],
    )
    assert gpio_modes_file_name(bare) is None


def test_a_modes_file_over_the_cap_is_refused_within_a_second() -> None:
    signal = _signal("TIM3_CH1", "GPIO_AF2_TIM3")
    data = _modes(("PA6", [signal] * (MAX_GPIO_MODES_BYTES // len(signal) + 1)))
    assert len(data) > MAX_GPIO_MODES_BYTES
    started = time.perf_counter()
    with pytest.raises(DescriptionError) as excinfo:
        read_gpio_alternates(data, file="modes.xml", pins={"PA6": frozenset({"TIM3_CH1"})})
    assert time.perf_counter() - started < 1.0
    assert excinfo.value.key == "<root>"


_FACTS = """
format = 1
rzone = "STM32_DIE000_16_64.xml"

[core]
fpu_double = false
dsp = true
trustzone = true
mpu_regions = 8
mpu_regions_secure = 4
sau_regions = 8
num_interrupts = 100
source = "a manual section"

[[regions]]
name = "Flash_NS"
start = 0x08000000
size = 0x10000
kind = "rom"
acc = { read = true, write = false, execute = true }
secure = false
source = "a manual figure"

[[regions]]
name = "SRAM_S"
start = 0x30000000
size = 0x4000
kind = "ram"
acc = { read = true, write = true, execute = true }
secure = true
physical = 0x20000000
source = "a manual table"
"""


def test_rzone_facts_read_as_the_rzone_the_builder_reads() -> None:
    rzone = read_rzone_facts(_FACTS.encode(), file="facts.toml")
    processor = rzone.processor
    assert (processor.fpu, processor.dsp, processor.tz) == (None, True, True)
    assert processor.mpu_regions - processor.sec_mpu_regions == 8
    assert (processor.sec_mpu_regions, processor.sau_regions) == (4, 8)
    assert processor.num_interrupts == 100
    assert (processor.clock_hz, processor.core, processor.mpu) == (None, None, None)
    assert rzone.regions == [
        RegionDesc(
            name="Flash_NS",
            start=0x0800_0000,
            size=0x1_0000,
            kind=RegionKind.ROM,
            acc=RegionAccess(read=True, write=False, execute=True),
            secure=False,
        ),
        RegionDesc(
            name="SRAM_S",
            start=0x3000_0000,
            size=0x4000,
            kind=RegionKind.RAM,
            acc=RegionAccess(read=True, write=True, execute=True),
            secure=True,
            physical=0x2000_0000,
        ),
    ]
    double = _FACTS.replace("fpu_double = false", "fpu_double = true")
    assert read_rzone_facts(double.encode(), file="facts.toml").processor.fpu == "DP_FPU"


@pytest.mark.parametrize(
    ("old", "new", "key"),
    [
        ("sau_regions = 8\n", "", "core.sau_regions"),
        ("size = 0x4000", "size = -1", "regions[1].size"),
        ('kind = "ram"', 'kind = "flash"', "regions[1].kind"),
        ('source = "a manual table"', 'source = " "', "regions[1].source"),
        ('source = "a manual section"', 'source = ""', "core.source"),
        ("format = 1", "format = 2", "format"),
        ("mpu_regions_secure = 4", "mpu_regions_secure = true", "core.mpu_regions_secure"),
        ("secure = true\n", "secure = true\ncontroller = 'FLASH'\n", "regions[1]"),
    ],
)
def test_rzone_facts_errors_name_the_key(old: str, new: str, key: str) -> None:
    assert _FACTS.count(old) == 1
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone_facts(_FACTS.replace(old, new).encode(), file="facts.toml")
    assert excinfo.value.key.startswith(key)


def test_malformed_rzone_facts_are_a_description_error() -> None:
    with pytest.raises(DescriptionError) as excinfo:
        read_rzone_facts(b"format = ", file="facts.toml")
    assert excinfo.value.key == "<root>"
