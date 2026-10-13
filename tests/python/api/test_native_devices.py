"""Native bus devices from Python and projects: popcorn.devices."""

from __future__ import annotations

import asyncio
import gc
import weakref
from collections.abc import Callable
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import ConfigError, StopReason, _core
from popcorn.devices import At25sf, Eeprom24xx, TableTarget, TableWrite
from popcorn.units import VirtualDuration, ms, ns, ps, us

from .devices_example import Eeprom24
from .projects import NATIVE_DEVICES
from .test_devices import BUDGET, I2C_ELF, SPI_ELF, enter, finish, pattern
from .test_devices import log as logged

CUSTOM = (0x1F, 0x87, 0x01)
PATTERN = bytes(pattern(i) for i in range(20))


def eeprom(write_cycle: VirtualDuration | None = None) -> Eeprom24xx:
    cycle = ms(5) if write_cycle is None else write_cycle
    return Eeprom24xx(0x50, size=4096, page_size=16, write_cycle=cycle)


def machine(
    u575: popcorn.PartDescription,
    elf: Path,
    bus: str,
    *devices: Eeprom24xx | TableTarget | Eeprom24,
) -> popcorn.Machine:
    return popcorn.Machine(u575, image=popcorn.Image.elf(elf), devices={bus: list(devices)})


def spi_machine(u575: popcorn.PartDescription, flash: At25sf) -> popcorn.Machine:
    return popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [flash]})


def run(m: popcorn.Machine, entry: str) -> list[int]:
    enter(m, entry)
    return finish(m)


def into_write_cycle(u575: popcorn.PartDescription, m: popcorn.Machine) -> None:
    """Run `m`, entered at eeprom_rw, to where the Python EEPROM of the same run NACKs its
    fourth poll, in the first write cycle."""
    probe = Eeprom24(0x50, 4096)
    reference = machine(u575, I2C_ELF, "I2C1", probe)
    enter(reference, "eeprom_rw")
    asyncio.run(reference.run(budget=BUDGET, stop_when=lambda _: probe.nacks > 3))
    asyncio.run(m.run(budget=reference.instructions - m.instructions))
    assert m.instructions == reference.instructions


def test_the_firmware_uses_a_native_eeprom_as_it_uses_the_python_one(
    u575: popcorn.PartDescription,
) -> None:
    native = eeprom()
    python = Eeprom24(0x50, 4096)
    result = run(machine(u575, I2C_ELF, "I2C1", native), "eeprom_rw")
    assert result[0] == 20
    assert result[1] > 0
    assert result == run(machine(u575, I2C_ELF, "I2C1", python), "eeprom_rw")
    assert native.contents()[0x10:0x24] == PATTERN
    assert native.contents() == bytes(python.array)
    assert native.address == 0x50
    assert isinstance(native, _core.I2CTargetBase)


def test_the_write_cycle_sets_how_long_the_firmware_polls(
    u575: popcorn.PartDescription,
) -> None:
    short = run(machine(u575, I2C_ELF, "I2C1", eeprom(ms(5))), "eeprom_rw")
    long = run(machine(u575, I2C_ELF, "I2C1", eeprom(ms(10))), "eeprom_rw")
    assert long[0] == short[0] == 20
    assert 1.8 * short[1] < long[1] < 2.2 * short[1]


def test_the_machine_board_lists_the_native_device_it_serves(
    u575: popcorn.PartDescription,
) -> None:
    device = eeprom()
    m = machine(u575, I2C_ELF, "I2C1", device)
    assert m.board.devices("I2C1") == (device,)
    assert m.board.devices("I2C1")[0] is device


@pytest.mark.parametrize(("cs", "entry"), [(None, "hw_ss"), ("PA4", "poll")])
def test_the_firmware_reads_the_jedec_id_of_a_custom_at25sf(
    u575: popcorn.PartDescription, cs: str | None, entry: str
) -> None:
    flash = At25sf(cs, jedec=CUSTOM, size=4 << 20)
    assert run(spi_machine(u575, flash), entry) == [0xFF, *CUSTOM]
    assert flash.cs == cs
    assert len(flash.contents()) == 4 << 20


def test_the_default_at25sf_is_an_at25sf641(u575: popcorn.PartDescription) -> None:
    flash = At25sf()
    assert run(spi_machine(u575, flash), "hw_ss") == [0xFF, 0x1F, 0x32, 0x17]
    assert len(flash.contents()) == 8 << 20


def test_a_page_program_lands_and_its_time_sets_the_busy_polls(
    u575: popcorn.PartDescription,
) -> None:
    fast = At25sf("PA4", page_program=us(600))
    slow = At25sf("PA4", page_program=us(1200))
    quick = run(spi_machine(u575, fast), "write_read")
    late = run(spi_machine(u575, slow), "write_read")
    assert quick[1] == late[1] == 16
    assert 1.8 * quick[0] < late[0] < 2.2 * quick[0]
    assert fast.contents()[0x1000:0x1010] == bytes(range(0xA0, 0xB0))


def test_a_sector_erase_time_sets_the_busy_polls(u575: popcorn.PartDescription) -> None:
    short = run(spi_machine(u575, At25sf("PA4", initial=b"\x00" * 0x2000, erase_4k=ms(1))), "erase")
    long = run(spi_machine(u575, At25sf("PA4", initial=b"\x00" * 0x2000, erase_4k=ms(2))), "erase")
    assert short[1] == long[1] == 16
    assert 1.8 * short[0] < long[0] < 2.2 * short[0]


def test_a_table_target_records_writes_and_answers_reads(
    u575: popcorn.PartDescription,
) -> None:
    table = TableTarget(0x38, register_bytes=2, replies={0x4002: [0x12, 0x34]})
    assert run(machine(u575, I2C_ELF, "I2C1", table), "table") == [0x12, 0x34]
    assert table.writes() == (TableWrite(0x4000, b"\x01"), TableWrite(0x4001, b"\x02\x03"))
    assert table.address == 0x38


@pytest.mark.parametrize(("nack_unknown", "log"), [(False, [0xFF, 0xFF]), (True, [])])
def test_a_read_from_a_register_without_a_reply_is_refused_on_request(
    u575: popcorn.PartDescription, nack_unknown: bool, log: list[int]
) -> None:
    table = TableTarget(0x38, register_bytes=2, nack_unknown=nack_unknown)
    m = machine(u575, I2C_ELF, "I2C1", table)
    enter(m, "table")
    stop = asyncio.run(m.run(budget=2_000_000))
    assert stop.reason is (StopReason.BUDGET if nack_unknown else StopReason.USER)
    assert logged(m) == log


def test_a_table_target_replies_accept_bytes() -> None:
    table = TableTarget(0x38, replies={1: b"\x05"})
    assert table.writes() == ()


def test_a_snapshot_mid_run_restores_into_a_fresh_machine_and_device(
    u575: popcorn.PartDescription,
) -> None:
    expected = run(machine(u575, I2C_ELF, "I2C1", eeprom()), "eeprom_rw")

    device = eeprom()
    m = machine(u575, I2C_ELF, "I2C1", device)
    enter(m, "eeprom_rw")
    fresh_state = eeprom().snapshot()
    into_write_cycle(u575, m)
    data = m.snapshot().to_bytes()
    state = device.snapshot()
    assert state != fresh_state

    copy = eeprom()
    other = machine(u575, I2C_ELF, "I2C1", copy)
    other.restore(popcorn.Snapshot.from_bytes(data))
    assert copy.snapshot() == state
    assert copy.contents() == device.contents()
    assert finish(other) == expected
    assert finish(m) == expected
    assert copy.contents() == device.contents()


def test_a_device_snapshot_round_trips_through_a_fresh_device(
    u575: popcorn.PartDescription,
) -> None:
    device = eeprom()
    run(machine(u575, I2C_ELF, "I2C1", device), "eeprom_rw")
    copy = eeprom()
    copy.restore(device.snapshot())
    assert copy.contents() == device.contents()
    assert copy.snapshot() == device.snapshot()

    flash = At25sf("PA4")
    run(spi_machine(u575, flash), "write_read")
    other = At25sf("PA4")
    other.restore(flash.snapshot())
    assert other.contents() == flash.contents()

    table = TableTarget(0x38, register_bytes=2, replies={0x4002: [0x12, 0x34]})
    run(machine(u575, I2C_ELF, "I2C1", table), "table")
    blank = TableTarget(0x38, register_bytes=2)
    blank.restore(table.snapshot())
    assert blank.writes() == table.writes()


def served(
    u575: popcorn.PartDescription,
) -> list[tuple[popcorn.Machine, Eeprom24xx | At25sf | TableTarget]]:
    device = eeprom()
    flash = At25sf("PA4")
    table = TableTarget(0x38, register_bytes=2)
    return [
        (machine(u575, I2C_ELF, "I2C1", device), device),
        (spi_machine(u575, flash), flash),
        (machine(u575, I2C_ELF, "I2C1", table), table),
    ]


def test_device_state_accessors_are_machine_running_while_its_machine_runs(
    u575: popcorn.PartDescription,
) -> None:
    async def scenario(m: popcorn.Machine, device: Eeprom24xx | At25sf | TableTarget) -> None:
        state = device.snapshot()
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                device.snapshot()
            with pytest.raises(popcorn.MachineRunning):
                device.restore(state)
            if not isinstance(device, TableTarget):
                with pytest.raises(popcorn.MachineRunning):
                    device.contents()

    for m, device in served(u575):
        enter(m, "idle")
        asyncio.run(scenario(m, device))
        assert not m.busy


def test_device_state_accessors_work_on_a_parked_machine(
    u575: popcorn.PartDescription,
) -> None:
    for m, device in served(u575):
        asyncio.run(m.run(budget=10_000))
        assert not m.busy
        state = device.snapshot()
        device.restore(state)
        assert device.snapshot() == state
        if not isinstance(device, TableTarget):
            assert set(device.contents()) == {0xFF}


def test_restore_refuses_the_state_of_another_size_or_initial_contents() -> None:
    state = At25sf(size=64 << 10).snapshot()
    with pytest.raises(ConfigError):
        At25sf(size=128 << 10).restore(state)
    with pytest.raises(ConfigError):
        At25sf(size=64 << 10, initial=b"\x00").restore(state)
    At25sf(size=64 << 10).restore(state)


def test_a_power_on_reset_mid_write_cycle_lets_the_next_run_start_fresh(
    u575: popcorn.PartDescription,
) -> None:
    expected = run(machine(u575, I2C_ELF, "I2C1", eeprom()), "eeprom_rw")
    device = eeprom()
    m = machine(u575, I2C_ELF, "I2C1", device)
    enter(m, "eeprom_rw")
    into_write_cycle(u575, m)
    assert run(m, "eeprom_rw") == expected
    assert device.contents()[0x10:0x24] == PATTERN


def test_initial_bytes_give_the_first_contents() -> None:
    flash = At25sf(size=64 << 10, initial=b"\x12\x34")
    assert flash.contents()[:3] == b"\x12\x34\xff"
    assert At25sf(size=64 << 10, initial=[1, 2]).contents()[:2] == b"\x01\x02"


def test_an_image_file_gives_the_first_contents(tmp_path: Path) -> None:
    image = tmp_path / "flash.bin"
    image.write_bytes(b"\xab\xcd")
    assert At25sf(size=64 << 10, image=image).contents()[:3] == b"\xab\xcd\xff"
    assert Eeprom24xx(size=256, address_bytes=1, image=str(image)).contents()[:3] == (
        b"\xab\xcd\xff"
    )


@pytest.mark.parametrize(
    ("make", "name"),
    [
        (lambda: At25sf(size=100_000), "^size:"),
        (lambda: At25sf(size=1 << 32), "^size:"),
        (lambda: At25sf(jedec=(1, 2)), "^jedec:"),
        (lambda: At25sf(jedec=(1, 2, 256)), "^jedec:"),
        (lambda: At25sf(device_id=-1), "^device_id:"),
        (lambda: At25sf(device_id=True), "^device_id:"),
        (lambda: At25sf(size=64 << 10, initial=bytes(65537)), "^initial:"),
        (lambda: At25sf(image="flash.bin", initial=b"\x00"), "^initial:"),
        (lambda: At25sf(image="/nonexistent/flash.bin"), "^image:"),
        (lambda: At25sf("PA16"), "chip select"),
        (lambda: At25sf(page_program=ps(1500)), "^page_program:"),
        (lambda: At25sf(reset=ns(-1)), "^reset:"),
        (lambda: Eeprom24xx(write_cycle=ps(1)), "^write_cycle:"),
        (lambda: Eeprom24xx(0x07), "I2C target address"),
        (lambda: Eeprom24xx(page_size=3), "^page_size:"),
        (lambda: Eeprom24xx(address_bytes=3), "^address_bytes:"),
        (lambda: Eeprom24xx(size=100), "^size:"),
        (lambda: TableTarget(0x78), "I2C target address"),
        (lambda: TableTarget(0x38, register_bytes=5), "^register_bytes:"),
        (lambda: TableTarget(0x38, replies={1: [256]}), "^replies:"),
    ],
)
def test_bad_arguments_are_value_errors_naming_the_parameter(
    make: Callable[[], object], name: str
) -> None:
    with pytest.raises(ValueError, match=name):
        make()


def test_a_duration_must_be_a_virtual_duration() -> None:
    with pytest.raises(TypeError, match="page_program"):
        At25sf(page_program=cast(VirtualDuration, 600))


def test_native_devices_are_checked_as_python_ones_are(
    u575: popcorn.PartDescription,
) -> None:
    with pytest.raises(ConfigError, match="an SPI peripheral for an SpiTarget") as refused:
        popcorn.Machine(u575, devices={"I2C1": [At25sf()]})
    assert refused.value.key == "devices.I2C1"
    with pytest.raises(ConfigError, match="an I2C peripheral for an I2CTarget"):
        popcorn.Machine(u575, devices={"SPI1": [eeprom()]})
    with pytest.raises(ConfigError, match="GPIO port"):
        popcorn.Machine(u575, devices={"SPI1": [At25sf("PZ4")]})
    device = eeprom()
    kept = machine(u575, I2C_ELF, "I2C1", device)
    with pytest.raises(ConfigError, match="not serving another live machine"):
        machine(u575, I2C_ELF, "I2C1", device)
    del kept
    gc.collect()
    machine(u575, I2C_ELF, "I2C1", device)


def test_the_machine_keeps_its_native_devices_alive(u575: popcorn.PartDescription) -> None:
    device = eeprom()
    held = weakref.ref(device)
    m = machine(u575, I2C_ELF, "I2C1", device)
    del device
    gc.collect()
    assert held() is not None
    assert run(m, "eeprom_rw")[0] == 20
    del m
    gc.collect()
    assert held() is None


def test_a_project_declares_a_custom_at25sf_on_the_hardware_ss(
    u575: popcorn.PartDescription,
) -> None:
    project = NATIVE_DEVICES
    (entry,) = project.board["SPI1"].devices
    assert entry.cs is None
    assert entry.params["page_program"] == us(600)
    m = project.machine()
    stop = asyncio.run(project.boot_machine(m))
    assert stop.reason is StopReason.ADDRESS
    m.reg.pc = m.sym("hw_ss")
    assert finish(m) == [0xFF, *CUSTOM]
    (flash,) = m.board.devices("SPI1")
    assert isinstance(flash, At25sf)
    assert flash.cs is None
    assert len(flash.contents()) == 4 << 20
    (second,) = project.machine().board.devices("SPI1")
    assert second is not flash


def test_a_project_device_takes_a_page_program_duration(
    u575: popcorn.PartDescription,
) -> None:
    project = NATIVE_DEVICES.replace(
        board={
            "SPI1": popcorn.Periph(
                devices=[popcorn.Device(At25sf, cs="PA4", page_program=us(1200))]
            )
        }
    )
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("write_read")
    slow = finish(m)
    quick = run(spi_machine(u575, At25sf("PA4", page_program=us(600))), "write_read")
    assert 1.8 * quick[0] < slow[0] < 2.2 * quick[0]


def test_a_project_device_takes_an_image_path(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    image = tmp_path / "flash.bin"
    image.write_bytes(b"\x5a\xa5")
    project = NATIVE_DEVICES.replace(
        board={"SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, image=image)])}
    )
    asyncio.run(project.boot_machine(project.machine()))
    (flash,) = project.machine().board.devices("SPI1")
    assert isinstance(flash, At25sf)
    assert flash.contents()[:3] == b"\x5a\xa5\xff"


def test_a_project_table_target_takes_integer_register_keys(
    u575: popcorn.PartDescription,
) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(I2C_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board={
            "I2C1": popcorn.Periph(
                devices=[
                    popcorn.Device(
                        TableTarget,
                        address=0x38,
                        register_bytes=2,
                        replies={0x4002: [0x12, 0x34], 7: [1]},
                    )
                ]
            )
        },
    )
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("table")
    assert finish(m) == [0x12, 0x34]
    (table,) = m.board.devices("I2C1")
    assert isinstance(table, TableTarget)
    assert table.writes() == (TableWrite(0x4000, b"\x01"), TableWrite(0x4001, b"\x02\x03"))


def test_a_project_integer_duration_is_refused() -> None:
    project = NATIVE_DEVICES.replace(
        board={"SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, page_program=600)])}
    )
    with pytest.raises(ConfigError, match="page_program") as refused:
        project.machine()
    assert refused.value.key == "board.SPI1.devices[0]"


def test_a_project_declares_native_i2c_devices(u575: popcorn.PartDescription) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(I2C_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board={
            "I2C1": popcorn.Periph(
                devices=[
                    popcorn.Device(
                        Eeprom24xx, address=0x50, size=4096, page_size=16, write_cycle=ms(5)
                    )
                ]
            )
        },
    )
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("eeprom_rw")
    assert finish(m) == run(machine(u575, I2C_ELF, "I2C1", eeprom()), "eeprom_rw")
    (device,) = m.board.devices("I2C1")
    assert isinstance(device, Eeprom24xx)
    assert device.contents()[0x10:0x24] == PATTERN


def bare_at25sf() -> _core.At25sfBase:
    return _core.At25sfBase(
        0x1F, 0x32, 0x17, 0x16, 64 << 10, "", b"", 600_000, 1, 1, 1, 1, 1, 1, 1, 1
    )


def test_a_bare_native_spi_base_is_refused_not_crashed(u575: popcorn.PartDescription) -> None:
    with pytest.raises(ConfigError, match="At25sf instances") as refused:
        popcorn.Machine(u575, devices={"SPI1": [cast(At25sf, bare_at25sf())]})
    assert refused.value.key == "devices.SPI1"
    config = _core.MachineConfig()
    config.python_devices = {"SPI1": [bare_at25sf()]}
    with pytest.raises(ConfigError, match="At25sf instances") as native:
        _core.MachineHandle(u575, config)
    assert native.value.key == "devices.SPI1"


def test_the_package_exports_the_devices_module() -> None:
    assert popcorn.devices.At25sf is At25sf


def test_a_bad_project_argument_is_refused_on_the_entry() -> None:
    project = NATIVE_DEVICES.replace(
        board={"SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, size=100)])}
    )
    with pytest.raises(ConfigError, match="size") as refused:
        project.machine()
    assert refused.value.key == "board.SPI1.devices[0]"
