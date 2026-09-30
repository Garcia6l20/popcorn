"""Python devices on the native I2C and SPI models: popcorn.board.I2CTarget and SpiTarget."""

from __future__ import annotations

import asyncio
import gc
import struct
import weakref
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import ConfigError, ModelError, ResetKind, StopReason, _core
from popcorn.board import I2CTarget, SpiTarget
from popcorn.model import Model
from popcorn.units import VirtualTime

from .devices_example import Eeprom24, JedecDevice

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
    u575: popcorn.MachineDescription, eeprom: I2CTarget | None = None
) -> tuple[popcorn.Machine, Eeprom24]:
    device = Eeprom24(0x50, 4096) if eeprom is None else eeprom
    assert isinstance(device, Eeprom24)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [device]})
    return m, device


def test_the_firmware_writes_and_reads_a_python_eeprom(u575: popcorn.MachineDescription) -> None:
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


def test_an_address_nobody_answers_is_nacked(u575: popcorn.MachineDescription) -> None:
    m, _ = eeprom_machine(u575)
    enter(m, "nack")
    assert finish(m) == [1, 1, 1, 0]


def test_the_firmware_reads_the_jedec_id_of_a_python_spi_device(
    u575: popcorn.MachineDescription,
) -> None:
    device = JedecDevice("PA4", (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]
    assert device.selects == [True, False]
    assert device.cs == "PA4"


def test_a_snapshot_mid_transfer_restores_into_a_fresh_machine_and_devices(
    u575: popcorn.MachineDescription,
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


def test_two_identical_runs_give_identical_snapshots(u575: popcorn.MachineDescription) -> None:
    states: list[bytes] = []
    for _ in range(2):
        m, _ = eeprom_machine(u575)
        enter(m, "eeprom_rw")
        finish(m)
        states.append(m.snapshot().to_bytes())
    assert states[0] == states[1]


def test_every_machine_reset_reaches_each_device_with_its_kind(
    u575: popcorn.MachineDescription,
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
    u575: popcorn.MachineDescription,
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


def test_the_default_reset_keeps_the_device_state(u575: popcorn.MachineDescription) -> None:
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
    u575: popcorn.MachineDescription, method: str
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


def test_device_time_restarts_on_power_on_only(u575: popcorn.MachineDescription) -> None:
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
    u575: popcorn.MachineDescription,
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


def test_a_device_on_the_hardware_ss_output(u575: popcorn.MachineDescription) -> None:
    device = JedecDevice(None, (0x1F, 0x87, 0x01))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "hw_ss")
    assert finish(m) == [0xFF, 0x1F, 0x87, 0x01]
    assert device.selects == [True, False]


@pytest.mark.parametrize("value", [256, -1, "x", 1.5])
def test_a_read_outside_a_byte_is_a_model_error(
    u575: popcorn.MachineDescription, value: object
) -> None:
    class Wrong(Eeprom24):
        def on_read(self, at: VirtualTime) -> int:
            return cast(int, value)

    m, _ = eeprom_machine(u575, Wrong(0x50, 4096))
    enter(m, "eeprom_rw")
    with pytest.raises(ModelError, match="on_read"):
        asyncio.run(m.run(budget=BUDGET))


def test_a_machine_keeps_its_devices_and_frees_them_with_it(
    u575: popcorn.MachineDescription,
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


def test_a_device_serves_one_live_machine(u575: popcorn.MachineDescription) -> None:
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
        (lambda: cast(dict[str, list[I2CTarget]], {"I2C1": [object()]}), "I2CTarget or SpiTarget"),
        (lambda: {"SPI1": [JedecDevice("PZ4", (1, 2, 3))]}, "GPIO port"),
    ],
)
def test_refused_devices_name_the_peripheral(
    u575: popcorn.MachineDescription, devices: _Devices, reason: str
) -> None:
    with pytest.raises(ConfigError, match=reason) as refused:
        popcorn.Machine(u575, devices=devices())
    assert refused.value.key.startswith("devices.")


def test_one_device_listed_twice_is_refused(u575: popcorn.MachineDescription) -> None:
    device = Eeprom24(0x50, 256)
    with pytest.raises(ConfigError, match="listed once"):
        popcorn.Machine(u575, devices={"I2C1": [device, device]})


def test_a_python_model_and_devices_on_one_peripheral_are_refused(
    u575: popcorn.MachineDescription,
) -> None:
    class Plain(Model):
        pass

    with pytest.raises(ConfigError, match="without a Python model") as refused:
        popcorn.Machine(u575, models={"I2C1": Plain}, devices={"I2C1": [Eeprom24(0x50, 256)]})
    assert refused.value.key == "devices.I2C1"


def test_an_acknowledge_that_is_not_a_bool_is_a_model_error(
    u575: popcorn.MachineDescription,
) -> None:
    class Loose(Eeprom24):
        def on_start(self, read: bool, at: VirtualTime) -> bool:
            return cast(bool, 1)

    m, _ = eeprom_machine(u575, Loose(0x50, 4096))
    enter(m, "eeprom_rw")
    with pytest.raises(ModelError, match="on_start"):
        asyncio.run(m.run(budget=BUDGET))


def test_an_spi_frame_wider_than_its_bits_is_a_model_error(
    u575: popcorn.MachineDescription,
) -> None:
    class Wide(JedecDevice):
        def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
            return 1 << bits

    device = Wide("PA4", (1, 2, 3))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    enter(m, "poll")
    with pytest.raises(ModelError, match="on_exchange"):
        asyncio.run(m.run(budget=BUDGET))


def test_a_plain_spi_target_returns_an_idle_miso(u575: popcorn.MachineDescription) -> None:
    m = popcorn.Machine(
        u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [SpiTarget("PA4")]}
    )
    enter(m, "poll")
    assert finish(m) == [0xFF, 0xFF, 0xFF, 0xFF]


def test_a_plain_i2c_target_acknowledges_and_reads_ff(u575: popcorn.MachineDescription) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [I2CTarget(0x50)]})
    enter(m, "eeprom_rw")
    result = finish(m)
    assert result[0] == 0
    assert result[1:] == [0, 0]


def test_a_device_defining_eq_is_attached_by_identity(u575: popcorn.MachineDescription) -> None:
    class Equal(I2CTarget):
        def __eq__(self, other: object) -> bool:
            return isinstance(other, Equal)

    first, second = Equal(0x50), Equal(0x51)
    m = popcorn.Machine(u575, devices={"I2C1": [first, second]})
    assert m.board.devices("I2C1") == (first, second)
    with pytest.raises(ConfigError, match="not serving another live machine"):
        popcorn.Machine(u575, devices={"I2C1": [second]})


def test_state_of_another_type_needs_its_own_snapshot(u575: popcorn.MachineDescription) -> None:
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
    u575: popcorn.MachineDescription,
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


def test_fr_per_5_a_project_file_wires_an_eeprom_the_firmware_uses(
    u575: popcorn.MachineDescription,
) -> None:
    project = popcorn.Project.load(DATA / "devices.toml")
    m = project.machine()
    stop = asyncio.run(project.boot(m))
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


def _project(tmp_path: Path, devices: str) -> Path:
    text = (DATA / "devices.toml").read_text()
    text = text.replace('"../../firmware/', f'"{DATA.parent.parent}/firmware/')
    text = text.replace('"../../python"', f'"{Path(__file__).parents[1]}"')
    start = text.index("[board.I2C1]")
    end = text.index("[models]")
    path = tmp_path / "popcorn.toml"
    path.write_text(text[:start] + devices + text[end:])
    return path


@pytest.mark.parametrize(
    ("devices", "key", "expected"),
    [
        (
            '[board.SPI1]\ndevices = [{ address = 0x50, model = "api.devices_example:Eeprom24", '
            "size = 16 }]\n",
            "board.SPI1.devices[0].address",
            "an I2C bus of STM32U575QGIx",
        ),
        (
            '[board.I2C1]\ndevices = [{ address = 0x07, model = "api.devices_example:Eeprom24", '
            "size = 16 }]\n",
            "board.I2C1.devices[0].address",
            "an I2C target address is 0x08 to 0x77, not 7",
        ),
        (
            '[board.I2C1]\ndevices = [{ address = 0x50, model = "api.devices_example:Eeprom24", '
            "size = 0 }]\n",
            "board.I2C1.devices[0]",
            "arguments Eeprom24 accepts: an EEPROM size is a positive multiple of its page",
        ),
        (
            '[board.I2C1]\nmodel = "api.models_example:Timer"\n'
            'devices = [{ address = 0x50, model = "api.devices_example:Eeprom24", size = 16 }]\n',
            "board.I2C1.devices",
            "no devices on a peripheral that has a Python model",
        ),
        (
            '[board.I2C1]\ndevices = [{ address = 0x50, model = "api.devices_example:JedecDevice", '
            "jedec = [1, 2, 3] }]\n",
            "board.I2C1.devices[0].model",
            "a popcorn.board.I2CTarget subclass on I2C",
        ),
    ],
)
def test_project_devices_are_refused_with_their_key(
    u575: popcorn.MachineDescription, tmp_path: Path, devices: str, key: str, expected: str
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Project.load(_project(tmp_path, devices)).machine()
    assert caught.value.key == key
    assert caught.value.expected == expected
    assert caught.value.line > 0
