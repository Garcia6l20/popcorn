"""Python devices on the native I2C and SPI models: popcorn.board.I2CTarget and SpiTarget."""

from __future__ import annotations

import asyncio
import gc
import struct
import weakref
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Literal, assert_type, cast

import popcorn
import pytest
from popcorn import ConfigError, ModelError, ResetKind, StopReason, _core
from popcorn.board import AudioOptions, DeviceBoard, I2CError, I2CTarget, SpiTarget
from popcorn.model import Model
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from . import models_example
from .conftest import run_bounded, stall, stalled
from .devices_example import Eeprom24, JedecDevice
from .projects import DEVICES

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
I2C_ELF = PREBUILT / "i2c_ll.elf"
SPI_ELF = PREBUILT / "spi_ll.elf"
BUDGET = 100_000_000


def pattern(index: int) -> int:
    return (7 * index + 3) & 0xFF


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    raw = m.mem.read(m.sym("log_buf"), 4 * min(count, 256))
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def enter(m: popcorn.Machine, entry: str, kind: ResetKind = ResetKind.POWER_ON) -> None:
    m.reset(kind)
    stop = asyncio.run(m.run(until="main", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS
    m.reg.pc = m.sym(entry)


def finish(m: popcorn.Machine) -> list[int]:
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER, stop
    return log(m)


def eeprom_machine(
    u575: popcorn.PartDescription, eeprom: I2CTarget | None = None
) -> tuple[popcorn.Machine, Eeprom24]:
    device = Eeprom24(0x50, 4096) if eeprom is None else eeprom
    assert isinstance(device, Eeprom24)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [device]})
    return m, device


def test_the_firmware_writes_and_reads_a_python_eeprom(u575: popcorn.PartDescription) -> None:
    m, eeprom = eeprom_machine(u575)
    enter(m, "eeprom_rw")
    result = finish(m)
    assert result[0] == 20
    assert result[1] > 0
    assert result[2] > 0
    assert eeprom.nacks == result[1] + result[2]
    assert bytes(eeprom.array[0x10:0x24]) == bytes(pattern(i) for i in range(20))
    assert m.board.devices("I2C1") == (eeprom,)
    assert m.board.devices("I2C2") == ()


def test_an_address_nobody_answers_is_nacked(u575: popcorn.PartDescription) -> None:
    m, _ = eeprom_machine(u575)
    enter(m, "nack")
    assert finish(m) == [1, 1, 1, 0]


def test_the_firmware_reads_the_jedec_id_of_a_python_spi_device(
    u575: popcorn.PartDescription,
) -> None:
    device = JedecDevice("PA4", (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]
    assert device.selects == [True, False]
    assert device.cs == "PA4"


def test_a_snapshot_mid_transfer_restores_into_a_fresh_machine_and_devices(
    u575: popcorn.PartDescription,
) -> None:
    fresh, _ = eeprom_machine(u575)
    enter(fresh, "eeprom_rw")
    expected = finish(fresh)

    m, eeprom = eeprom_machine(u575)
    enter(m, "eeprom_rw")
    stop = asyncio.run(m.run(budget=BUDGET, stop_when=lambda _: eeprom.nacks > 3))
    assert stop.reason is StopReason.USER
    data = m.snapshot().to_bytes()

    other, copy = eeprom_machine(u575)
    other.restore(popcorn.Snapshot.from_bytes(data))
    assert copy.busy_until == eeprom.busy_until
    assert copy.array == eeprom.array
    assert finish(other) == expected
    assert finish(m) == expected
    assert copy.array == eeprom.array


def test_two_identical_runs_give_identical_snapshots(u575: popcorn.PartDescription) -> None:
    states: list[bytes] = []
    for _ in range(2):
        m, _ = eeprom_machine(u575)
        enter(m, "eeprom_rw")
        finish(m)
        states.append(m.snapshot().to_bytes())
    assert states[0] == states[1]


def test_every_machine_reset_reaches_each_device_with_its_kind(
    u575: popcorn.PartDescription,
) -> None:
    eeprom = Eeprom24(0x50, 4096)
    other = Eeprom24(0x51, 256)
    m = popcorn.Machine(u575, devices={"I2C1": [eeprom, other]})
    assert eeprom.resets == []
    m.reset()
    m.reset(ResetKind.SOFTWARE)
    assert eeprom.resets == [ResetKind.POWER_ON, ResetKind.SOFTWARE]
    assert other.resets == eeprom.resets


def test_a_power_on_reset_during_twr_lets_the_eeprom_answer_at_once(
    u575: popcorn.PartDescription,
) -> None:
    fresh, _ = eeprom_machine(u575)
    enter(fresh, "eeprom_rw")
    expected = finish(fresh)

    m, eeprom = eeprom_machine(u575)
    enter(m, "eeprom_rw")
    stop = asyncio.run(m.run(budget=BUDGET, stop_when=lambda _: eeprom.nacks > 0))
    assert stop.reason is StopReason.USER
    written = bytes(eeprom.array)
    enter(m, "eeprom_rw")
    assert eeprom.busy_until == 0
    assert bytes(eeprom.array) == written
    assert finish(m) == expected


def test_the_default_reset_keeps_the_device_state(u575: popcorn.PartDescription) -> None:
    class Counter(I2CTarget):
        count: int

        def __init__(self) -> None:
            super().__init__(0x50)
            self.count = 7

    counter = Counter()
    m = popcorn.Machine(u575, devices={"I2C1": [counter]})
    m.reset()
    counter.count = 9
    m.reset()
    assert counter.count == 9
    assert counter.snapshot() == Counter().snapshot().replace(b"\x07", b"\x09")


class Flaky(Eeprom24):
    def __init__(self) -> None:
        super().__init__(0x50, 4096)
        self.broken = ""

    def check(self, method: str) -> None:
        if self.broken == method:
            raise RuntimeError(f"{method} broke")

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        self.check("on_start")
        return super().on_start(read, at)

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        self.check("on_write")
        return super().on_write(byte, at)

    def on_read(self, at: VirtualTime) -> int:
        self.check("on_read")
        return super().on_read(at)


@pytest.mark.parametrize("method", ["on_start", "on_write", "on_read"])
def test_a_device_exception_reaches_the_run_and_a_restore_recovers(
    u575: popcorn.PartDescription, method: str
) -> None:
    fresh, _ = eeprom_machine(u575)
    enter(fresh, "eeprom_rw")
    expected = finish(fresh)

    flaky = Flaky()
    m, _ = eeprom_machine(u575, flaky)
    enter(m, "eeprom_rw")
    before = m.snapshot()
    flaky.broken = method
    with pytest.raises(RuntimeError, match=f"{method} broke") as raised:
        asyncio.run(m.run(budget=BUDGET))
    assert raised.traceback[-1].name == "check"
    assert raised.traceback[-2].name == method
    flaky.broken = ""
    m.restore(before)
    assert finish(m) == expected


class Stamped(Eeprom24):
    def __init__(self) -> None:
        super().__init__(0x50, 4096)
        self.starts: list[int] = []

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        self.starts.append(at.ps)
        return super().on_start(read, at)


def test_device_time_restarts_on_power_on_only(u575: popcorn.PartDescription) -> None:
    stamped = Stamped()
    m, _ = eeprom_machine(u575, stamped)
    enter(m, "eeprom_rw")
    finish(m)
    first = list(stamped.starts)
    assert first[0] > 0

    stamped.starts.clear()
    enter(m, "eeprom_rw", ResetKind.SOFTWARE)
    finish(m)
    assert stamped.starts[0] > first[-1]

    stamped.starts.clear()
    enter(m, "eeprom_rw")
    finish(m)
    assert stamped.starts == first


def test_an_spi_snapshot_mid_transfer_restores_into_a_fresh_device(
    u575: popcorn.PartDescription,
) -> None:
    device = JedecDevice("PA4", (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    stop = asyncio.run(m.run(budget=BUDGET, stop_when=lambda _: device.position == 2))
    assert stop.reason is StopReason.USER
    data = m.snapshot().to_bytes()

    copy = JedecDevice("PA4", (0x1F, 0x87, 0x01))
    other = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [copy]})
    other.restore(popcorn.Snapshot.from_bytes(data))
    assert (copy.position, copy.command) == (2, 0x9F)
    assert finish(other) == [0xFF, 0x1F, 0x87, 0x01]
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]


def test_a_device_on_the_hardware_ss_output(u575: popcorn.PartDescription) -> None:
    device = JedecDevice(None, (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "hw_ss")
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]
    assert device.selects == [True, False]


@pytest.mark.parametrize("value", [256, -1, "x", 1.5])
def test_a_read_outside_a_byte_is_a_model_error(
    u575: popcorn.PartDescription, value: object
) -> None:
    class Wrong(Eeprom24):
        def on_read(self, at: VirtualTime) -> int:
            return cast(int, value)

    m, _ = eeprom_machine(u575, Wrong(0x50, 4096))
    enter(m, "eeprom_rw")
    with pytest.raises(ModelError, match="on_read"):
        asyncio.run(m.run(budget=BUDGET))


def test_a_machine_keeps_its_devices_and_frees_them_with_it(
    u575: popcorn.PartDescription,
) -> None:
    device = Eeprom24(0x50, 256)
    watch = weakref.ref(device)
    m = popcorn.Machine(u575, devices={"I2C1": [device]})
    del device
    gc.collect()
    assert watch() is not None
    del m
    gc.collect()
    assert watch() is None


def test_a_device_serves_one_live_machine(u575: popcorn.PartDescription) -> None:
    device = Eeprom24(0x50, 256)
    m = popcorn.Machine(u575, devices={"I2C1": [device]})
    with pytest.raises(ConfigError) as refused:
        popcorn.Machine(u575, devices={"I2C1": [device]})
    assert refused.value.key == "devices.I2C1"
    del m
    gc.collect()
    again = popcorn.Machine(u575, devices={"I2C1": [device]})
    assert again.board.devices("I2C1") == (device,)


def test_bad_addresses_and_chip_selects_are_value_errors() -> None:
    for address in (0x07, 0x78, True):
        with pytest.raises(ValueError, match="0x08 to 0x77"):
            I2CTarget(address)
    for cs in ("A4", "PA16", "pa4", "PA", ""):
        with pytest.raises(ValueError, match="pin name"):
            SpiTarget(cs)
    assert SpiTarget().cs is None


type _Devices = Callable[[], Mapping[str, Sequence[I2CTarget | SpiTarget]]]


@pytest.mark.parametrize(
    ("devices", "reason"),
    [
        (lambda: {"NOPE": [Eeprom24(0x50, 256)]}, "a peripheral of"),
        (lambda: {"USART1": [Eeprom24(0x50, 256)]}, "an I2C or SPI peripheral"),
        (lambda: {"RCC": list[I2CTarget]()}, "an I2C or SPI peripheral"),
        (lambda: {"SPI1": [Eeprom24(0x50, 256)]}, "an I2C peripheral for an I2CTarget"),
        (lambda: {"I2C1": [JedecDevice(None, (1, 2, 3))]}, "an SPI peripheral for an SpiTarget"),
        (
            lambda: cast(dict[str, list[I2CTarget]], {"I2C1": [object()]}),
            "I2CTarget, SpiTarget or SpiController",
        ),
        (lambda: {"SPI1": [JedecDevice("PZ4", (1, 2, 3))]}, "GPIO port"),
    ],
)
def test_refused_devices_name_the_peripheral(
    u575: popcorn.PartDescription, devices: _Devices, reason: str
) -> None:
    with pytest.raises(ConfigError, match=reason) as refused:
        popcorn.Machine(u575, devices=devices())
    assert refused.value.key.startswith("devices.")


def test_one_device_listed_twice_is_refused(u575: popcorn.PartDescription) -> None:
    device = Eeprom24(0x50, 256)
    with pytest.raises(ConfigError, match="listed once"):
        popcorn.Machine(u575, devices={"I2C1": [device, device]})


def test_a_python_model_and_devices_on_one_peripheral_are_refused(
    u575: popcorn.PartDescription,
) -> None:
    class Plain(Model):
        pass

    with pytest.raises(ConfigError, match="without a Python model") as refused:
        popcorn.Machine(u575, models={"I2C1": Plain}, devices={"I2C1": [Eeprom24(0x50, 256)]})
    assert refused.value.key == "devices.I2C1"


def test_an_acknowledge_that_is_not_a_bool_is_a_model_error(
    u575: popcorn.PartDescription,
) -> None:
    class Loose(Eeprom24):
        def on_start(self, read: bool, at: VirtualTime) -> bool:
            return cast(bool, 1)

    m, _ = eeprom_machine(u575, Loose(0x50, 4096))
    enter(m, "eeprom_rw")
    with pytest.raises(ModelError, match="on_start"):
        asyncio.run(m.run(budget=BUDGET))


def test_an_spi_frame_wider_than_its_bits_is_a_model_error(
    u575: popcorn.PartDescription,
) -> None:
    class Wide(JedecDevice):
        def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
            return 1 << bits

    device = Wide("PA4", (1, 2, 3))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    with pytest.raises(ModelError, match="on_exchange"):
        asyncio.run(m.run(budget=BUDGET))


def test_a_plain_spi_target_returns_an_idle_miso(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(
        u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [SpiTarget("PA4")]}
    )
    enter(m, "poll")
    assert finish(m) == [0xFF, 0xFF, 0xFF, 0xFF]


def test_a_plain_i2c_target_acknowledges_and_reads_ff(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [I2CTarget(0x50)]})
    enter(m, "eeprom_rw")
    result = finish(m)
    assert result[0] == 0
    assert result[1:] == [0, 0]


def test_a_device_defining_eq_is_attached_by_identity(u575: popcorn.PartDescription) -> None:
    class Equal(I2CTarget):
        def __eq__(self, other: object) -> bool:
            return isinstance(other, Equal)

    first, second = Equal(0x50), Equal(0x51)
    m = popcorn.Machine(u575, devices={"I2C1": [first, second]})
    assert m.board.devices("I2C1") == (first, second)
    with pytest.raises(ConfigError, match="not serving another live machine"):
        popcorn.Machine(u575, devices={"I2C1": [second]})


def test_state_of_another_type_needs_its_own_snapshot(u575: popcorn.PartDescription) -> None:
    class Plain(I2CTarget):
        table: dict[int, int]

        def __init__(self) -> None:
            super().__init__(0x50)
            self.table = {}

    class Custom(Plain):
        def snapshot(self) -> bytes:
            return bytes(sorted(self.table))

        def restore(self, data: bytes) -> None:
            self.table = dict.fromkeys(data, 0)

    m = popcorn.Machine(u575, devices={"I2C1": [Plain()]})
    with pytest.raises(TypeError, match="override snapshot"):
        m.snapshot()
    custom = Custom()
    custom.table = {3: 0, 1: 0}
    other = popcorn.Machine(u575, devices={"I2C1": [custom]})
    taken = other.snapshot()
    custom.table = {}
    other.restore(taken)
    assert custom.table == {1: 0, 3: 0}


@pytest.mark.parametrize(
    ("peripheral", "device", "reason"),
    [
        ("RCC", lambda: I2CTarget(0x50), "an I2C or SPI peripheral"),
        ("SPI1", lambda: I2CTarget(0x50), "an SPI peripheral"),
        ("I2C1", lambda: SpiTarget(), "an I2C peripheral"),
        ("I2C1", lambda: object(), "an I2C peripheral"),
    ],
)
def test_the_native_layer_refuses_devices_on_its_own(
    u575: popcorn.PartDescription,
    peripheral: str,
    device: Callable[[], object],
    reason: str,
) -> None:
    config = _core.MachineConfig()
    config.python_devices = {peripheral: [device()]}
    with pytest.raises(ConfigError, match=reason) as refused:
        _core.MachineHandle(u575, config)
    assert refused.value.key == f"devices.{peripheral}"


DATA = Path(__file__).parents[2] / "data" / "api"


def test_fr_per_5_a_project_wires_an_eeprom_the_firmware_uses(
    u575: popcorn.PartDescription,
) -> None:
    project = DEVICES
    m = project.machine()
    stop = asyncio.run(project.boot_machine(m))
    assert stop.reason is StopReason.ADDRESS
    m.reg.pc = m.sym("eeprom_rw")
    result = finish(m)
    assert result[0] == 20
    (eeprom,) = m.board.devices("I2C1")
    assert isinstance(eeprom, Eeprom24)
    assert eeprom.address == 0x50
    assert bytes(eeprom.array[0x10:0x24]) == bytes(pattern(i) for i in range(20))
    other = project.machine()
    (second,) = other.board.devices("I2C1")
    assert isinstance(second, Eeprom24)
    assert second is not eeprom
    assert second.array == bytearray(b"\xff" * 4096)


def eeprom_entry(
    *, address: int | None = 0x50, cs: str | None = None, size: int = 16
) -> popcorn.Device:
    return popcorn.Device(Eeprom24, address=address, cs=cs, size=size)


def jedec_entry(*, address: int | None = None, cs: str | None = None) -> popcorn.Device:
    return popcorn.Device(JedecDevice, address=address, cs=cs, jedec=[1, 2, 3])


@pytest.mark.parametrize(
    ("board", "key", "expected"),
    [
        (
            {"SPI1": popcorn.Periph(devices=[eeprom_entry()])},
            "board.SPI1.devices[0].model",
            "an I2C bus of STM32U575QGIx",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[eeprom_entry(address=0x07)])},
            "board.I2C1.devices[0].address",
            "an I2C target address is 0x08 to 0x77, not 7",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[eeprom_entry(size=0)])},
            "board.I2C1.devices[0]",
            "arguments Eeprom24 accepts: an EEPROM size is a positive multiple of its page",
        ),
        (
            {
                "I2C1": popcorn.Periph(
                    model=popcorn.Use(models_example.Timer), devices=[eeprom_entry()]
                )
            },
            "board.I2C1.devices",
            "no devices on a peripheral that has a Python model",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[jedec_entry(cs="PA4")])},
            "board.I2C1.devices[0].model",
            "an SPI bus of STM32U575QGIx",
        ),
        (
            {"SPI1": popcorn.Periph(devices=[jedec_entry(address=0x50)])},
            "board.SPI1.devices[0].address",
            "no address for an SPI device, it has a cs",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[eeprom_entry(address=None)])},
            "board.I2C1.devices[0].address",
            "the address of an I2C target",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[eeprom_entry(cs="PA4")])},
            "board.I2C1.devices[0].cs",
            "an address for an I2C target or a cs for an SPI device, not both",
        ),
        (
            {"I2C1": popcorn.Periph(devices=[eeprom_entry(address=None, cs="PA4")])},
            "board.I2C1.devices[0].cs",
            "no cs for an I2C target",
        ),
        (
            {"SPI1": popcorn.Periph(devices=[jedec_entry(cs="PZ4")])},
            "board.SPI1.devices[0].cs",
            "a chip select on a GPIO port of STM32U575QGIx",
        ),
    ],
)
def test_project_devices_are_refused_with_their_key(
    u575: popcorn.PartDescription,
    board: dict[str, popcorn.Periph],
    key: str,
    expected: str,
) -> None:
    with pytest.raises(ConfigError) as caught:
        DEVICES.replace(board=board).machine()
    assert caught.value.key == key
    assert caught.value.expected == expected


@pytest.mark.parametrize(("cs", "entry"), [("PA4", "poll"), (None, "hw_ss")])
def test_a_project_wires_spi_devices_by_chip_select(
    u575: popcorn.PartDescription, cs: str | None, entry: str
) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(SPI_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board={
            "SPI1": popcorn.Periph(
                devices=[popcorn.Device(JedecDevice, cs=cs, jedec=[0x1F, 0x87, 0x01])]
            )
        },
    )
    assert project.board["SPI1"].devices[0].cs == cs
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym(entry)
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]
    (device,) = m.board.devices("SPI1")
    assert isinstance(device, JedecDevice)
    assert device.cs == cs


def _two_bus_project() -> popcorn.MachineDescription:
    base = DEVICES.replace(
        board={
            "I2C1": popcorn.Periph(devices=[popcorn.Device(Eeprom24, address=0x50, size=4096)]),
            "SPI1": popcorn.Periph(devices=[popcorn.Device(JedecDevice, cs="PA4")]),
        }
    )
    return base.with_configs(slow=base.replace(cpi=2.0))


def test_project_devices_replace_those_of_their_peripheral_only(
    u575: popcorn.PartDescription,
) -> None:
    project = _two_bus_project()
    given = Eeprom24(0x50, 4096)
    m = project.machine(devices={"I2C1": [given]})
    assert m.config_digest == project.digest
    assert m.board.devices("I2C1") == (given,)
    (kept,) = m.board.devices("SPI1")
    assert isinstance(kept, JedecDevice)
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("eeprom_rw")
    assert finish(m)[0] == 20
    assert bytes(given.array[0x10:0x24]) == bytes(pattern(i) for i in range(20))


def test_project_devices_through_an_alias_a_config_or_an_empty_list(
    u575: popcorn.PartDescription,
) -> None:
    project = _two_bus_project()
    given = Eeprom24(0x50, 4096)
    assert project.machine(devices={"SEC_I2C1": [given]}).board.devices("I2C1") == (given,)
    other = Eeprom24(0x50, 4096)
    slow = project.config("slow").machine(devices={"I2C1": [other]})
    assert slow.config_digest != project.digest
    assert slow.board.devices("I2C1") == (other,)
    assert project.machine(devices={"SPI1": []}).board.devices("SPI1") == ()


@pytest.mark.parametrize(
    ("devices", "key", "expected"),
    [
        ({"I2C9": [Eeprom24(0x50, 16)]}, "devices.I2C9", "a peripheral of STM32U575QGIx"),
        ({"USART1": [Eeprom24(0x50, 16)]}, "devices.USART1", "an I2C or SPI peripheral"),
        (
            {"I2C1": [Eeprom24(0x50, 16)], "SEC_I2C1": [Eeprom24(0x51, 16)]},
            "devices.SEC_I2C1",
            "each peripheral once",
        ),
    ],
)
def test_refused_project_devices_name_their_key(
    u575: popcorn.PartDescription,
    devices: dict[str, list[I2CTarget]],
    key: str,
    expected: str,
) -> None:
    with pytest.raises(ConfigError) as refused:
        _two_bus_project().machine(devices=devices)
    assert refused.value.key == key
    assert refused.value.expected == expected


def test_project_devices_given_for_a_peripheral_are_not_built(
    u575: popcorn.PartDescription,
) -> None:
    broken = DEVICES.replace(board={"I2C1": popcorn.Periph(devices=[eeprom_entry(size=0)])})
    given = Eeprom24(0x50, 16)
    assert broken.machine(devices={"I2C1": [given]}).board.devices("I2C1") == (given,)


def clocked_machine(u575: popcorn.PartDescription, *devices: I2CTarget) -> popcorn.Machine:
    return popcorn.Machine(
        u575,
        devices={"I2C1": list(devices)},
        audio={"SEC_SAI1": {"A": AudioOptions(clocked_by="I2C1:0x38")}},
    )


def test_a_device_drives_the_clock_of_the_sai_block_wired_to_it_only(
    u575: popcorn.PartDescription,
) -> None:
    codec, other = I2CTarget(0x38), I2CTarget(0x39)
    m = clocked_machine(u575, codec, other)
    assert isinstance(codec.board, DeviceBoard)
    codec.board.set_audio_clock("SAI1", "A", 16_000)
    assert m.periph.SAI1.audio("A").external_frame_hz == 16_000
    codec.board.set_audio_clock("SEC_SAI1", "A", 0)
    assert m.periph.SAI1.audio("A").external_frame_hz == 0
    for device, sai, block in (
        (other, "SAI1", "A"),
        (codec, "SAI1", "B"),
        (codec, "SAI2", "A"),
        (codec, "USART1", "A"),
    ):
        with pytest.raises(ConfigError) as refused:
            device.board.set_audio_clock(sai, cast(Literal["A", "B"], block), 8_000)
        assert refused.value.key.endswith(".clocked_by"), (device, sai, block)
    assert m.periph.SAI1.audio("A").external_frame_hz == 0
    assert m.periph.SAI1.audio("B").external_frame_hz == 0
    with pytest.raises(ValueError, match=r"2\*\*32"):
        codec.board.set_audio_clock("SAI1", "A", -1)


def test_a_device_board_without_a_live_machine_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    codec = I2CTarget(0x38)
    with pytest.raises(ConfigError):
        codec.board.set_audio_clock("SAI1", "A", 16_000)
    m = clocked_machine(u575, codec)
    kept = codec.board
    watch = weakref.ref(m)
    del m
    gc.collect()
    assert watch() is None
    with pytest.raises(ConfigError):
        kept.set_audio_clock("SAI1", "A", 16_000)
    again = clocked_machine(u575, codec)
    kept.set_audio_clock("SAI1", "A", 16_000)
    assert again.periph.SAI1.audio("A").external_frame_hz == 16_000


@pytest.mark.parametrize(
    ("clocked_by", "key"),
    [
        ("I2C1:0x39", "audio.SAI1.A.clocked_by"),
        ("I2C2:0x38", "audio.SAI1.A.clocked_by"),
        ("I2C1", "audio.SAI1.A.clocked_by"),
        ("I2C1:size", "audio.SAI1.A.clocked_by"),
        ("SPI1:PA4", "audio.SAI1.A.clocked_by"),
        (cast(str, 0x38), "audio.SAI1.A.clocked_by"),
    ],
)
def test_a_clocked_by_naming_no_attached_device_is_refused_at_build(
    u575: popcorn.PartDescription, clocked_by: str, key: str
) -> None:
    codec = I2CTarget(0x38)
    with pytest.raises(ConfigError) as refused:
        popcorn.Machine(
            u575,
            devices={"I2C1": [codec], "SPI1": [SpiTarget()]},
            audio={"SAI1": {"A": AudioOptions(clocked_by=clocked_by)}},
        )
    assert refused.value.key == key
    assert popcorn.Machine(u575, devices={"I2C1": [codec]}).board.devices("I2C1") == (codec,)


def test_clocked_by_names_an_spi_device_by_its_chip_select(
    u575: popcorn.PartDescription,
) -> None:
    on_ss, on_pin = SpiTarget(), SpiTarget("PA4")
    m = popcorn.Machine(
        u575,
        devices={"SPI1": [on_ss, on_pin]},
        audio={
            "SAI1": {
                "A": AudioOptions(clocked_by="SPI1:SS"),
                "B": AudioOptions(clocked_by="SPI1:PA4"),
            }
        },
    )
    on_ss.board.set_audio_clock("SAI1", "A", 48_000)
    on_pin.board.set_audio_clock("SAI1", "B", 44_100)
    assert m.periph.SAI1.audio("A").external_frame_hz == 48_000
    assert m.periph.SAI1.audio("B").external_frame_hz == 44_100
    with pytest.raises(ConfigError):
        on_pin.board.set_audio_clock("SAI1", "A", 0)


def test_clocked_by_names_a_bus_through_its_alias(u575: popcorn.PartDescription) -> None:
    codec = I2CTarget(0x38)
    m = popcorn.Machine(
        u575,
        devices={"I2C1": [codec]},
        audio={"SAI1": {"A": AudioOptions(clocked_by="SEC_I2C1:0x38")}},
    )
    codec.board.set_audio_clock("SAI1", "A", 16_000)
    assert m.periph.SAI1.audio("A").external_frame_hz == 16_000


@pytest.mark.parametrize("method", ["on_start", "on_write", "on_read"])
def test_an_i2c_device_error_inside_running_raises_at_the_next_wait(
    u575: popcorn.PartDescription, method: str
) -> None:
    flaky = Flaky()
    flaky.broken = method
    m, _ = eeprom_machine(u575, flaky)
    enter(m, "eeprom_rw")

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("main"), for_=ms(100))

    with pytest.raises(RuntimeError, match=f"{method} broke"):
        run_bounded(scenario())
    assert not m.busy


class BrokenJedec(JedecDevice):
    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        raise RuntimeError("exchange broke")


def test_an_spi_device_error_inside_running_raises_at_the_next_wait(
    u575: popcorn.PartDescription,
) -> None:
    device = BrokenJedec("PA4", (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("main"), for_=ms(100))

    with pytest.raises(RuntimeError, match="exchange broke"):
        run_bounded(scenario())
    assert not m.busy


class Marker(I2CTarget):
    """An I2C target that answers nothing, a second kind on a bus."""


def test_board_device_returns_the_one_device_of_a_class_typed(
    u575: popcorn.PartDescription,
) -> None:
    eeprom = Eeprom24(0x50, 4096)
    marker = Marker(0x51)
    m = popcorn.Machine(u575, devices={"I2C1": [eeprom, marker]})
    found = m.board.device("I2C1", Eeprom24)
    assert_type(found, Eeprom24)
    assert found is eeprom
    assert m.board.device("I2C1", Marker) is marker
    assert m.board.device("SEC_I2C1", Eeprom24) is eeprom
    assert m.board.devices("SEC_I2C1") == m.board.devices("I2C1") == (eeprom, marker)


def test_board_device_matches_a_subclass_instance(u575: popcorn.PartDescription) -> None:
    eeprom = Eeprom24(0x50, 4096)
    m = popcorn.Machine(u575, devices={"I2C1": [eeprom]})
    assert m.board.device("I2C1", I2CTarget) is eeprom


@pytest.mark.parametrize(("peripheral", "cls"), [("I2C1", I2CTarget), ("I2C2", Eeprom24)])
def test_board_device_refuses_none_or_several(
    u575: popcorn.PartDescription, peripheral: str, cls: type[I2CTarget]
) -> None:
    m = popcorn.Machine(u575, devices={"I2C1": [Eeprom24(0x50, 4096), Marker(0x51)]})
    with pytest.raises(ConfigError) as raised:
        m.board.device(peripheral, cls)
    assert raised.value.key == f"board.{peripheral}"


def test_board_device_names_a_peripheral_the_part_lacks(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, devices={"I2C1": [Eeprom24(0x50, 4096)]})
    with pytest.raises(ConfigError, match="a peripheral of") as raised:
        m.board.device("I2C9", Eeprom24)
    assert raised.value.key == "board.I2C9"


ERROR_BITS = {
    I2CError.BUS_ERROR: 1 << 8,
    I2CError.ARBITRATION_LOST: 1 << 9,
    I2CError.OVERRUN: 1 << 10,
}
HOOKS = [("on_start", "errors", 0), ("on_write", "errors", 1), ("on_read", "read_errors", 1)]


class Faulty(I2CTarget):
    """Answers 0x33 and returns `error` from `hook` once, on call `at_call` of a transfer: 0 is
    the address, 1 the first data byte."""

    def __init__(self, hook: str, error: object, at_call: int) -> None:
        super().__init__(0x33)
        self.hook = hook
        self.error = error
        self.at_call = at_call
        self.shots = 1
        self.calls = 0
        self.events: list[tuple[str, int, int]] = []

    def pick[T](self, hook: str, normal: T) -> T:
        if hook != self.hook or self.calls != self.at_call or self.shots == 0:
            return normal
        self.shots -= 1
        return cast(T, self.error)

    def on_start(self, read: bool, at: VirtualTime) -> bool | I2CError:
        self.calls = 0
        self.events.append(("R" if read else "W", 0, at.ps))
        return self.pick("on_start", True)

    def on_write(self, byte: int, at: VirtualTime) -> bool | I2CError:
        self.calls += 1
        self.events.append(("w", byte, at.ps))
        return self.pick("on_write", True)

    def on_read(self, at: VirtualTime) -> int | I2CError:
        self.calls += 1
        self.events.append(("r", 0x5A, at.ps))
        return self.pick("on_read", 0x5A)

    def on_stop(self, at: VirtualTime) -> None:
        self.events.append(("P", 0, at.ps))


def faulty_machine(
    u575: popcorn.PartDescription, hook: str, error: object, entry: str, at_call: int
) -> tuple[popcorn.Machine, Faulty]:
    device = Faulty(hook, error, at_call)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [device]})
    enter(m, entry)
    return m, device


def expected_log(error: I2CError, entry: str) -> list[int]:
    busy = 0 if error is I2CError.ARBITRATION_LOST else 1
    data = [0x5A, 0x5A] if entry == "read_errors" else []
    return [2, 1, ERROR_BITS[error], busy, 0, 0, *data]


@pytest.mark.parametrize("error", list(I2CError), ids=lambda e: e.name)
@pytest.mark.parametrize(("hook", "entry", "at_call"), HOOKS, ids=[h for h, _, _ in HOOKS])
def test_an_i2c_error_returned_by_a_hook_reaches_the_firmware_error_handler(
    u575: popcorn.PartDescription, error: I2CError, hook: str, entry: str, at_call: int
) -> None:
    m, device = faulty_machine(u575, hook, error, entry, at_call)
    assert finish(m) == expected_log(error, entry)
    kinds = "".join(kind for kind, _, _ in device.events[-4:])
    assert kinds == ("Rrr" if entry == "read_errors" else "Www") + "P"


@pytest.mark.parametrize(
    ("hook", "value"),
    [
        ("on_start", "BERR"),
        ("on_start", 1),
        ("on_write", "BERR"),
        ("on_write", 3),
        ("on_write", None),
        ("on_write", ResetKind.PIN),
        ("on_read", "BERR"),
        ("on_read", None),
        ("on_read", ResetKind.PIN),
    ],
)
def test_a_hook_returning_neither_its_value_nor_an_i2c_error_is_a_model_error(
    u575: popcorn.PartDescription, hook: str, value: object
) -> None:
    entry, at_call = next((e, c) for h, e, c in HOOKS if h == hook)
    m, _ = faulty_machine(u575, hook, value, entry, at_call)
    with pytest.raises(ModelError, match=hook):
        asyncio.run(m.run(budget=BUDGET))


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


@pytest.mark.parametrize("error", [None, I2CError.BUS_ERROR], ids=["plain", "error"])
def test_each_i2c_hook_call_takes_the_gil_once_with_or_without_an_error(
    u575: popcorn.PartDescription, error: I2CError | None
) -> None:
    m, device = faulty_machine(u575, "on_write", error or True, "errors", 1)
    before = gil_entries(m)
    finish(m)
    starts = sum(kind in "RW" for kind, _, _ in device.events)
    assert gil_entries(m) - before == len(device.events) + starts


@pytest.mark.timeout(40)
def test_an_injected_arbitration_loss_is_alike_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    m, parked_device = faulty_machine(u575, "on_write", I2CError.ARBITRATION_LOST, "errors", 1)
    parked_log = finish(m)
    m, running_device = faulty_machine(u575, "on_write", I2CError.ARBITRATION_LOST, "errors", 1)

    async def body() -> None:
        async with m.running():
            await m.wait_for(m.ref.var("log_len").ge(7))

    with pytest.raises(popcorn.EmulationError, match=r"USER at .* errors"):
        run_bounded(body(), 30)
    assert log(m) == parked_log == expected_log(I2CError.ARBITRATION_LOST, "errors")
    assert running_device.events == parked_device.events


class Stretching(I2CTarget):
    """Answers the table address, records each call, and stretches SCL by `for_` once, on the
    `nth` call of `hook`, counted from 0 over the whole run."""

    def __init__(self, hook: str = "", nth: int = 0, for_: object = None) -> None:
        super().__init__(0x38)
        self.hook = hook
        self.nth = nth
        self.for_ = for_
        self.counts: dict[str, int] = {}
        self.events: list[tuple[str, int]] = []
        self.stretched_at = -1

    def note(self, hook: str, at: VirtualTime) -> None:
        count = self.counts.get(hook, 0)
        self.counts[hook] = count + 1
        self.events.append((hook, at.ps))
        if hook == self.hook and count == self.nth:
            self.stretched_at = len(self.events) - 1
            self.stretch(cast(VirtualDuration, self.for_))

    def on_start(self, read: bool, at: VirtualTime) -> bool | I2CError:
        self.note("on_start", at)
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool | I2CError:
        self.note("on_write", at)
        return True

    def on_read(self, at: VirtualTime) -> int | I2CError:
        self.note("on_read", at)
        return 0x11

    def on_stop(self, at: VirtualTime) -> None:
        self.events.append(("on_stop", at.ps))


def stretched_table(u575: popcorn.PartDescription, target: Stretching) -> list[int]:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [target]})
    enter(m, "table")
    return finish(m)


STRETCHES = [("on_write", 1), ("on_start", 3), ("on_read", 0)]


@pytest.mark.parametrize(("hook", "nth"), STRETCHES, ids=[h for h, _ in STRETCHES])
def test_a_stretch_inside_a_hook_delays_the_next_bus_phase_by_its_duration(
    u575: popcorn.PartDescription, hook: str, nth: int
) -> None:
    plain = Stretching()
    expected = stretched_table(u575, plain)
    target = Stretching(hook, nth, us(10))
    assert stretched_table(u575, target) == expected
    index = target.stretched_at
    assert target.events[index][0] == hook
    assert [kind for kind, _ in target.events] == [kind for kind, _ in plain.events]

    def gap(events: list[tuple[str, int]]) -> int:
        return events[index + 1][1] - events[index][1]

    assert gap(target.events) == gap(plain.events) + us(10).ps
    assert target.events[: index + 1] == plain.events[: index + 1]


def test_a_stretch_outside_a_hook_or_of_a_bad_duration_is_refused() -> None:
    target = I2CTarget(0x38)
    with pytest.raises(RuntimeError, match="on_start, on_write or on_read"):
        target.stretch(us(10))
    with pytest.raises(TypeError, match="VirtualDuration"):
        target.stretch(cast(VirtualDuration, 5))
    with pytest.raises(ValueError, match="0 to 1 s"):
        target.stretch(ms(2000))
    with pytest.raises(ValueError, match="0 to 1 s"):
        target.stretch(us(-1))


@pytest.mark.parametrize(
    ("value", "error"), [(5, TypeError), (ms(2000), ValueError)], ids=["type", "range"]
)
def test_a_bad_stretch_inside_a_hook_stops_the_run_and_resets_the_hook_state(
    u575: popcorn.PartDescription, value: object, error: type[Exception]
) -> None:
    target = Stretching("on_write", 0, value)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [target]})
    enter(m, "table")
    with pytest.raises(error):
        asyncio.run(m.run(budget=BUDGET))
    with pytest.raises(RuntimeError, match="on_start, on_write or on_read"):
        target.stretch(us(1))


def test_a_stretching_hook_takes_the_gil_once_per_call(u575: popcorn.PartDescription) -> None:
    target = Stretching("on_write", 1, us(10))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [target]})
    enter(m, "table")
    before = gil_entries(m)
    finish(m)
    assert target.stretched_at >= 0
    starts = sum(kind == "on_start" for kind, _ in target.events)
    assert gil_entries(m) - before == len(target.events) + starts


HOLD_AT = ms(2)
HOLD_FOR = ms(1)
ADDRESS_PS = 10 * 2_500_000


def held_eeprom(u575: popcorn.PartDescription) -> tuple[popcorn.Machine, Stamped, VirtualTime]:
    target = Stamped()
    m, _ = eeprom_machine(u575, target)
    enter(m, "eeprom_rw")
    return m, target, m.time + HOLD_AT


def check_held_starts(starts: list[int], at: VirtualTime) -> None:
    end = (at + HOLD_FOR).ps
    assert any(t < at.ps for t in starts)
    assert not any(at.ps + ADDRESS_PS < t < end + ADDRESS_PS for t in starts)
    assert end + ADDRESS_PS in starts


def test_a_held_bus_delays_the_firmware_start_to_the_hold_end(
    u575: popcorn.PartDescription,
) -> None:
    m, target, at = held_eeprom(u575)
    events = m.pending_events
    assert asyncio.run(target.hold_bus(at=at, for_=HOLD_FOR)) == at
    assert m.pending_events == events + 2
    assert finish(m)[0] == 20
    check_held_starts(target.starts, at)


@pytest.mark.timeout(40)
def test_a_hold_is_alike_parked_and_running(u575: popcorn.PartDescription) -> None:
    m, parked, at = held_eeprom(u575)
    asyncio.run(parked.hold_bus(at=at, for_=HOLD_FOR))
    parked_log = finish(m)
    m, running, at = held_eeprom(u575)
    held = stall(m, m.time + us(1))

    async def post(boundary: VirtualTime) -> VirtualTime:
        assert boundary < at
        return await running.hold_bus(at=at, for_=HOLD_FOR)

    async def body() -> None:
        async with m.running():
            _, start = await stalled(m, post, held=held)
            assert start == at
            await m.wait_for(m.ref.var("log_len").ge(4))

    with pytest.raises(popcorn.EmulationError, match=r"USER at .* eeprom_rw"):
        run_bounded(body(), 30)
    assert log(m) == parked_log
    assert running.starts == parked.starts
    check_held_starts(running.starts, at)


def test_a_hold_of_a_target_serving_no_machine_is_a_config_error(
    u575: popcorn.PartDescription,
) -> None:
    target = I2CTarget(0x38)
    with pytest.raises(ConfigError, match="serves no live machine"):
        target.hold_bus(for_=ms(1))
    m = popcorn.Machine(u575, devices={"I2C1": [target]})
    asyncio.run(target.hold_bus(for_=ms(1)))
    machine = weakref.ref(m)
    del m
    gc.collect()
    assert machine() is None
    with pytest.raises(ConfigError, match="serves no live machine"):
        target.hold_bus(for_=ms(1))


def test_a_hold_keeps_neither_the_machine_nor_the_target_alive(
    u575: popcorn.PartDescription,
) -> None:
    target = I2CTarget(0x38)
    m = popcorn.Machine(u575, devices={"I2C1": [target]})
    asyncio.run(target.hold_bus(at=m.time + ms(1), for_=ms(1)))
    watched = weakref.ref(m), weakref.ref(target)
    del m, target
    gc.collect()
    assert [w() for w in watched] == [None, None]


@pytest.mark.parametrize(
    ("for_", "at", "error"),
    [
        (5, None, TypeError),
        (ms(1), 5, TypeError),
        (ms(0), None, ValueError),
        (VirtualDuration(-1), None, ValueError),
        (ms(1), VirtualTime(-1), ValueError),
        (ms(1), VirtualTime((1 << 64) - 2), ValueError),
        (VirtualDuration(1 << 64), None, ValueError),
    ],
    ids=["for-type", "at-type", "zero", "negative", "at-negative", "overflow", "for-overflow"],
)
def test_a_bad_hold_is_refused_at_the_call_with_nothing_posted(
    u575: popcorn.PartDescription, for_: object, at: object, error: type[Exception]
) -> None:
    target = I2CTarget(0x38)
    m = popcorn.Machine(u575, devices={"I2C1": [target]})
    events = m.pending_events
    with pytest.raises(error):
        target.hold_bus(for_=cast(VirtualDuration, for_), at=cast(VirtualTime | None, at))
    assert m.pending_events == events


def test_a_hold_in_the_past_of_a_parked_machine_is_refused_and_posts_nothing(
    u575: popcorn.PartDescription,
) -> None:
    m, target, at = held_eeprom(u575)
    assert m.time.ps > 0
    events = m.pending_events
    with pytest.raises(ValueError, match="before the machine time"):
        asyncio.run(target.hold_bus(at=VirtualTime(0), for_=HOLD_FOR))
    assert m.pending_events == events
    assert asyncio.run(target.hold_bus(at=at, for_=HOLD_FOR)) == at
    assert finish(m)[0] == 20
    check_held_starts(target.starts, at)


def test_a_thousand_replaced_holds_leave_two_events(u575: popcorn.PartDescription) -> None:
    target = I2CTarget(0x38)
    m = popcorn.Machine(u575, devices={"I2C1": [target]})
    events = m.pending_events
    for index in range(1000):
        asyncio.run(target.hold_bus(at=m.time + us(index + 1), for_=ms(1)))
    assert m.pending_events == events + 2


def test_a_hold_takes_no_gil_on_the_machine_thread(u575: popcorn.PartDescription) -> None:
    target = I2CTarget(0x38)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [target]})
    enter(m, "idle")
    asyncio.run(target.hold_bus(at=m.time + us(10), for_=us(100)))
    before = gil_entries(m)
    asyncio.run(m.run(for_=ms(1)))
    assert gil_entries(m) == before


class Holding(Stamped):
    def on_start(self, read: bool, at: VirtualTime) -> bool:
        self.hold_bus(for_=ms(1))
        return True


def test_a_hold_from_a_hook_is_refused_from_the_run(u575: popcorn.PartDescription) -> None:
    m, _ = eeprom_machine(u575, Holding())
    enter(m, "eeprom_rw")
    with pytest.raises(RuntimeError, match="a handler accesses the machine directly"):
        asyncio.run(m.run(budget=BUDGET))


class Moded(SpiTarget):
    """Answers all ones and records each frame, declaring the SPI mode `mode`."""

    def __init__(self, cs: str | None = None, *, mode: int | None = None) -> None:
        super().__init__(cs, mode=mode)
        self.frames: list[int] = []

    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        self.frames.append(out)
        return (1 << bits) - 1


MODE_REFUSED = (
    r"devices\.SPI1, expected the SPI mode 3 the device on PA4 declares, "
    r"not mode 0 the firmware set, CPOL 0 CPHA 0"
)


@pytest.mark.parametrize(
    ("mode", "error"),
    [(4, ValueError), (-1, ValueError), (True, TypeError), ("0", TypeError), (1.0, TypeError)],
)
def test_an_spi_mode_outside_0_to_3_is_refused_at_construction(
    mode: object, error: type[Exception]
) -> None:
    with pytest.raises(error, match="mode"):
        SpiTarget("PA4", mode=cast(int, mode))


def test_a_device_declaring_the_firmware_spi_mode_runs_as_one_declaring_none(
    u575: popcorn.PartDescription,
) -> None:
    logs: list[list[int]] = []
    entries: list[int] = []
    for mode in (None, 0):
        device = Moded("PA4", mode=mode)
        m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
        enter(m, "poll")
        before = gil_entries(m)
        logs.append(finish(m))
        entries.append(gil_entries(m) - before)
        assert device.frames == [0x9F, 0, 0, 0]
    assert logs[0] == logs[1] == [0xFF] * 4
    assert entries[0] == entries[1] == 6


def test_a_device_declaring_another_spi_mode_fails_the_run_after_its_first_frame(
    u575: popcorn.PartDescription,
) -> None:
    device = Moded("PA4", mode=3)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    with pytest.raises(ConfigError, match=MODE_REFUSED) as raised:
        asyncio.run(m.run(budget=BUDGET))
    assert raised.value.key == "devices.SPI1"
    assert device.frames == [0x9F]
    assert finish(m) == [0xFF] * 4
    assert device.frames == [0x9F, 0, 0, 0]


@pytest.mark.timeout(40)
def test_an_spi_mode_mismatch_inside_running_raises_at_the_next_wait(
    u575: popcorn.PartDescription,
) -> None:
    device = Moded("PA4", mode=3)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("main"), for_=ms(100))

    with pytest.raises(ConfigError, match=MODE_REFUSED):
        run_bounded(scenario(), 30)
    assert not m.busy
    assert device.frames == [0x9F]


def test_a_project_device_passes_its_spi_mode_through(u575: popcorn.PartDescription) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(SPI_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board={"SPI1": popcorn.Periph(devices=[popcorn.Device(Moded, cs="PA4", mode=3)])},
    )
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("poll")
    with pytest.raises(ConfigError, match=MODE_REFUSED):
        asyncio.run(m.run(budget=BUDGET))


class Stateful(SpiTarget):
    """A device keeping its own state in `_mode`, a name a subclass may well use."""

    def __init__(self, cs: str | None = None) -> None:
        super().__init__(cs)
        self._mode = "idle"


def test_a_subclass_attribute_named_mode_declares_no_spi_mode(
    u575: popcorn.PartDescription,
) -> None:
    device = Stateful("PA4")
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    assert finish(m) == [0xFF] * 4


def test_a_machine_restored_after_a_mode_mismatch_resumes_without_a_second_error(
    u575: popcorn.PartDescription,
) -> None:
    device = Moded("PA4", mode=3)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    with pytest.raises(ConfigError, match=MODE_REFUSED):
        asyncio.run(m.run(budget=BUDGET))
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    copy = Moded("PA4", mode=3)
    other = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [copy]})
    other.restore(saved)
    assert finish(other) == [0xFF] * 4
    assert copy.frames == [0, 0, 0]
    assert finish(m) == [0xFF] * 4


def test_a_mode_mismatch_on_a_linked_system_member_raises_from_the_system_run(
    u575: popcorn.PartDescription,
) -> None:
    a = popcorn.Machine(
        u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [Moded("PA4", mode=3)]}, name="a"
    )
    b = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), name="b")
    enter(a, "poll")
    enter(b, "idle")
    link = popcorn.UartLink("a.USART1", "b.USART1", latency=us(1), baud=115_200)
    with popcorn.System({"a": a, "b": b}, [link]) as system:
        with pytest.raises(ConfigError, match=MODE_REFUSED) as raised:
            asyncio.run(system.run(for_=ms(5)))
        assert raised.value.key == "devices.SPI1"
