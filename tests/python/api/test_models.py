"""Python behaviour models: handlers by register name, events, IRQs, snapshots, lifetime."""

from __future__ import annotations

import asyncio
import functools
import gc
import itertools
import logging
import traceback
import warnings
import weakref
from collections.abc import Callable, Mapping
from pathlib import Path

import popcorn
import pytest
from popcorn.model import IrqLine, Model, on_read, on_write
from popcorn.units import VirtualTime, ms, us

from .conftest import run_bounded
from .models_example import Eeprom, Timer
from .projects import MODEL_ELF, MODEL_JSON, Plain, model_project
from .projects import Eeprom as FakeEeprom
from .projects import Timer as FakeTimer


def machine(
    models: Mapping[str, Callable[[], Model]], *, strict_peripherals: bool = False
) -> popcorn.Machine:
    """A machine over the model firmware, booted to `ready` with interrupts enabled."""
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models=models,
        strict_peripherals=strict_peripherals,
    )
    m.reset()
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    return m


def call(m: popcorn.Machine, fn: str, *args: int, interrupts: bool = False) -> int:
    result = asyncio.run(m.call(fn, *args, budget=1_000_000, interrupts=interrupts))
    assert result.stop.reason is popcorn.StopReason.RETURNED, result.stop
    return int(result.value)


def timer_of(m: popcorn.Machine) -> Timer:
    model = m.model("TIMER")
    assert isinstance(model, Timer)
    return model


def test_a_write_handler_sees_old_and_new_and_other_registers_keep_storage() -> None:
    m = machine({"TIMER": Timer})
    call(m, "write_timer_cr", 0)
    call(m, "start_timer", 25)
    assert timer_of(m).writes == [(0, 0), (0, 1)]
    assert m.periph.TIMER.ARR.value == 25
    assert m.periph.TIMER.CR.value == 1


def test_a_scheduled_event_raises_the_irq_the_firmware_counts() -> None:
    m = machine({"TIMER": Timer})
    call(m, "start_timer", 10)
    assert call(m, "wait_timer_irqs", 3, interrupts=True) == 3
    assert m.mem.u32[m.sym("timer_irqs")] == 3
    fired = timer_of(m).fired
    assert [b.ps - a.ps for a, b in itertools.pairwise(fired)] == [10_000_000] * 2


def test_the_eeprom_answers_a_write_then_read_sequence() -> None:
    m = machine({"EEPROM": Eeprom})
    assert call(m, "eeprom_roundtrip", 0x10, 0xDEADBEEF) == 0xDEADBEEF
    model = m.model("EEPROM")
    assert isinstance(model, Eeprom)
    assert model.memory[0x10:0x14] == bytes.fromhex("efbeadde")


def test_a_bus_write_from_python_reaches_the_model() -> None:
    m = machine({"TIMER": Timer})
    m.periph.TIMER.CR.write(1)
    m.mem.u32[0x4003_0000] = 0
    assert timer_of(m).writes == [(0, 1), (1, 0)]
    assert m.periph.TIMER.CNT.read() == 0


def test_snapshot_and_restore_bring_back_fields_and_pending_events() -> None:
    m = machine({"TIMER": Timer})
    call(m, "start_timer", 10)
    saved = m.snapshot()
    timer = timer_of(m)
    asyncio.run(m.run(for_=us(25)))
    first = list(timer.fired)
    assert timer.expirations == 2
    m.restore(saved)
    assert timer.expirations == 0
    timer.fired.clear()
    asyncio.run(m.run(for_=us(25)))
    assert timer.fired == first
    assert timer.expirations == 2


def test_a_reset_returns_declared_fields_to_their_constructed_values() -> None:
    m = machine({"TIMER": Timer})
    timer_of(m).expirations = 7
    m.reset()
    assert timer_of(m).expirations == 0


def test_a_handler_exception_wins_over_a_fault_in_the_same_slice() -> None:
    class Failing(Timer):
        @on_write("CR")
        def _explode(self, old: int, new: int) -> None:
            raise LookupError(f"CR written with {new}")

    m = machine({"TIMER": Failing})
    with pytest.raises(LookupError, match="CR written with 5") as caught:
        asyncio.run(m.call("write_then_fault", 5))
    assert any(frame.name == "_explode" for frame in traceback.extract_tb(caught.tb))


def test_an_on_read_exception_reaches_the_caller() -> None:
    class Failing(Timer):
        @on_read("CNT")
        def _broken(self) -> int:
            raise ArithmeticError("no count")

    m = machine({"TIMER": Failing})
    with pytest.raises(ArithmeticError, match="no count"):
        call(m, "read_timer_count")


def test_a_read_handler_returning_a_non_int_is_a_model_error() -> None:
    class Wrong(Model):
        @on_read("CNT")
        def _cnt(self) -> int:
            return "three"  # pyright: ignore[reportReturnType]

    m = machine({"TIMER": Wrong})
    with pytest.raises(popcorn.ModelError) as caught:
        call(m, "read_timer_count")
    assert (caught.value.peripheral, caught.value.register, caught.value.offset) == (
        "TIMER",
        "CNT",
        12,
    )


def test_a_value_the_bus_cannot_carry_is_a_model_error() -> None:
    class Wide(Model):
        @on_read("CNT")
        def _cnt(self) -> int:
            return 0

        def _read(self, offset: int, size: int) -> int | None:
            return 1 << 40

    m = machine({"TIMER": Wide})
    with pytest.raises(popcorn.ModelError) as caught:
        call(m, "read_timer_count")
    assert (caught.value.peripheral, caught.value.register) == ("TIMER", "CNT")


def test_handlers_refuse_an_access_with_a_bus_error() -> None:
    class Refusing(Model):
        @on_write("CR")
        def _cr(self, old: int, new: int) -> bool:
            return False

        @on_read("CNT")
        def _cnt(self) -> int | None:
            return None

    m = machine({"TIMER": Refusing})
    written = asyncio.run(m.call("write_timer_cr", 1, budget=10_000))
    assert written.stop.reason is popcorn.StopReason.FAULT
    assert m.periph.TIMER.CR.value == 0
    m = machine({"TIMER": Refusing})
    read = asyncio.run(m.call("read_timer_count", budget=10_000))
    assert read.stop.reason is popcorn.StopReason.FAULT


def test_sub_word_accesses_see_the_whole_register() -> None:
    m = machine({"TIMER": Timer})
    timer = timer_of(m)
    timer.expirations = 0x1234
    assert (m.mem.u8[0x4003_000C], m.mem.u8[0x4003_000D], m.mem.u16[0x4003_000E]) == (
        0x34,
        0x12,
        0,
    )
    m.mem.u8[0x4003_0001] = 0x56
    assert timer.writes == [(0, 0x5600)]
    assert m.periph.TIMER.CR.read() == 0x5600


def test_a_register_view_write_reaches_the_python_model() -> None:
    class Addressed(Model):
        def __init__(self) -> None:
            super().__init__()
            self.written: list[tuple[int, int]] = []

        @on_write("ADDR")
        def _set_addr(self, old: int, new: int) -> None:
            self.written.append((old, new))

    m = machine({"EEPROM": Addressed})
    model = m.model("EEPROM")
    assert isinstance(model, Addressed)
    m.periph.EEPROM.ADDR.value = 0x1234
    assert model.written == [(0, 0x1234)]
    m.periph.EEPROM.ADDR.poke(0x99)
    assert model.written == [(0, 0x1234)]


def test_registers_sharing_a_word_dispatch_to_their_own_handlers() -> None:
    class Halves(Model):
        lengths: int = 0

        def __init__(self) -> None:
            super().__init__()
            self.written: list[tuple[int, int]] = []

        @on_read("LEN")
        def _len(self) -> int:
            return 0x7_0003

        @on_write("LEN")
        def _set_len(self, old: int, new: int) -> None:
            self.written.append((old, new))

        @on_write("ADDR")
        def _set_addr(self, old: int, new: int) -> None:
            self.written.append((-old, -new))

    m = machine({"EEPROM": Halves})
    model = m.model("EEPROM")
    assert isinstance(model, Halves)
    m.periph.EEPROM.ADDR.poke(0x1234)
    assert m.mem.u32[0x4004_0008] == 0x0003_1234
    m.mem.u16[0x4004_000A] = 0x5678
    assert model.written == [(0, 0x5678)]
    m.mem.u16[0x4004_0008] = 0x1111
    assert model.written == [(0, 0x5678), (-0x1234, -0x1111)]
    m.mem.u32[0x4004_0008] = 0xAAAA_BBBB
    assert sorted(model.written[2:]) == [(-0x1111, -0xBBBB), (0x5678, 0xAAAA)]
    assert m.periph.EEPROM.ADDR.value == 0xBBBB


def test_an_unmodelled_access_under_strict_rules_is_a_model_error() -> None:
    m = machine({}, strict_peripherals=True)
    with pytest.raises(popcorn.ModelError) as caught:
        call(m, "touch_unmodelled")
    assert (caught.value.peripheral, caught.value.register, caught.value.offset) == (
        "UNMODELLED",
        "DATA",
        0,
    )


def test_a_strict_free_run_logs_its_unmodelled_access_before_the_model_error_is_raised(
    caplog: pytest.LogCaptureFixture,
) -> None:
    m = machine({}, strict_peripherals=True)
    m.reg.lr = m.sym("ready") | 1
    m.reg.pc = m.sym("touch_unmodelled") & ~1
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m.start()
        with pytest.raises(popcorn.ModelError):
            asyncio.run(m.wait_for(m.ref.mem.u32[m.sym("sink")].eq(1), for_=ms(1)))
        logged = [r for r in caplog.records if getattr(r, "popcorn_register", None) == "DATA"]
        assert len(logged) == 1
        with pytest.raises(popcorn.ModelError):
            m.halt()
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def test_dropping_the_machine_frees_the_model() -> None:
    m = machine({"TIMER": Timer})
    call(m, "start_timer", 10)
    ref = weakref.ref(timer_of(m))
    del m
    gc.collect()
    assert ref() is None


def test_a_model_kept_past_its_machine_refuses_machine_access() -> None:
    m = machine({"TIMER": Timer})
    timer = timer_of(m)
    del m
    gc.collect()
    with pytest.raises(popcorn.ConfigError, match="machine still exists"):
        _ = timer.now


def test_a_run_without_python_models_calls_no_model_code() -> None:
    def count_model_calls(m: popcorn.Machine) -> int:
        before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
        asyncio.run(m.run(for_=us(100)))
        return m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]

    plain = machine({})
    call(plain, "start_timer", 10)
    assert count_model_calls(plain) == 0
    modelled = machine({"TIMER": Timer})
    call(modelled, "start_timer", 10)
    assert count_model_calls(modelled) > 0


def test_a_registered_model_serves_every_peripheral_of_its_ip() -> None:
    popcorn.model.register("pytimer", "v*")(Timer)
    try:
        m = machine({})
    finally:
        popcorn.model.unregister("pytimer", "v*")
    assert isinstance(m.model("TIMER"), Timer)
    assert m.model("EEPROM") is None
    assert machine({}).model("TIMER") is None


def test_register_refuses_a_class_that_is_not_a_model() -> None:
    with pytest.raises(TypeError, match=r"not a popcorn\.model\.Model"):
        popcorn.model.register("pytimer")(int)  # pyright: ignore[reportArgumentType]


def test_model_parameters_go_to_the_constructor() -> None:
    class Scaled(Timer):
        def __init__(self, scale: int) -> None:
            super().__init__()
            self.scale = scale

    m = machine({"TIMER": functools.partial(Scaled, scale=3)})
    model = m.model("TIMER")
    assert isinstance(model, Scaled) and model.scale == 3


def test_models_must_name_a_peripheral() -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        machine({"TIMR": Timer})
    assert caught.value.key == "models.TIMR"
    assert "TIMER" in str(caught.value)


def test_a_claim_of_an_unknown_register_is_refused() -> None:
    class Stray(Model):
        @on_write("NOPE")
        def _nope(self, old: int, new: int) -> None: ...

    with pytest.raises(popcorn.ConfigError) as caught:
        machine({"TIMER": Stray})
    assert caught.value.key == "peripherals.TIMER.registers"


def test_a_maker_must_return_a_model() -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        machine({"TIMER": object})  # pyright: ignore[reportArgumentType]
    assert caught.value.key == "models.TIMER"


def test_a_model_is_only_built_by_its_machine() -> None:
    with pytest.raises(TypeError, match="built by its machine"):
        Timer()


def test_a_model_must_initialise_its_base() -> None:
    class Forgetful(Model):
        def __init__(self) -> None:
            pass

    with pytest.raises(TypeError, match="super"):
        machine({"TIMER": Forgetful})


def test_more_line_fields_than_lines_are_refused() -> None:
    class Greedy(Model):
        first: IrqLine
        second: IrqLine

    with pytest.raises(popcorn.ConfigError) as caught:
        machine({"TIMER": Greedy})
    assert caught.value.key == "TIMER.second"


def test_state_without_a_default_encoding_needs_its_own_snapshot() -> None:
    class Listed(Model):
        items: list[int]

    class Custom(Model):
        items: list[int]

        def __init__(self) -> None:
            super().__init__()
            self.items = [1, 2]

        def snapshot(self) -> bytes:
            return bytes(self.items)

        def restore(self, data: bytes) -> None:
            self.items = list(data)

    with pytest.raises(TypeError, match="override snapshot"):
        machine({"TIMER": Listed})
    m = machine({"TIMER": Custom})
    saved = m.snapshot()
    model = m.model("TIMER")
    assert isinstance(model, Custom)
    model.items = [9]
    m.restore(saved)
    assert model.items == [1, 2]


def test_every_state_type_round_trips() -> None:
    class Typed(Model):
        flag: bool = True
        small: int = -3
        edge: int = 255
        big: int = 1 << 90
        ratio: float = 0.25
        raw: bytes = b"\x00\xff"
        buffer: bytearray
        text: str = "caf\u00e9"

        def __init__(self) -> None:
            super().__init__()
            self.buffer = bytearray(b"ab")

    m = machine({"TIMER": Typed})
    model = m.model("TIMER")
    assert isinstance(model, Typed)
    saved = model.snapshot()
    model.flag, model.small, model.big, model.ratio = False, 0, 0, 0.0
    model.raw, model.buffer, model.text = b"", bytearray(), ""
    model.edge = 0
    model.restore(saved)
    assert (model.flag, model.small, model.big, model.ratio) == (True, -3, 1 << 90, 0.25)
    assert model.edge == 255
    assert (model.raw, model.buffer, model.text) == (b"\x00\xff", bytearray(b"ab"), "caf\u00e9")
    assert isinstance(model.buffer, bytearray)
    with pytest.raises(popcorn.ModelError):
        model.restore(saved + b"\x00")
    with pytest.raises(popcorn.ModelError):
        model.restore(saved[:-1])
    model.small = "x"  # pyright: ignore[reportAttributeAccessIssue]
    with pytest.raises(popcorn.ModelError, match="small"):
        model.snapshot()


def test_schedule_takes_methods_of_the_model_only() -> None:
    m = machine({"TIMER": Timer})
    timer = timer_of(m)
    with pytest.raises(TypeError, match="not a method"):
        timer.schedule(us(1), lambda: None)
    with pytest.raises(ValueError, match="in the past"):
        timer.schedule(us(-1), timer.reset_count)
    with pytest.raises(TypeError, match="not a method"):
        timer.schedule(us(1), timer_of(machine({"TIMER": Timer})).reset_count)


def test_a_cancelled_event_does_not_fire() -> None:
    m = machine({"TIMER": Timer})
    timer = timer_of(m)
    handle = timer.schedule(us(5), timer.reset_count)
    timer.cancel(handle)
    timer.expirations = 4
    asyncio.run(m.run(for_=us(10)))
    assert timer.expirations == 4
    timer.schedule(us(5), timer.reset_count)
    asyncio.run(m.run(for_=us(10)))
    assert timer.expirations == 0


def test_register_store_and_field_masks() -> None:
    m = machine({"TIMER": Timer})
    timer = timer_of(m)
    timer.regs.ARR = 0x1234
    assert m.periph.TIMER.ARR.value == 0x1234
    assert timer.fields.SR.UIF == 1
    with pytest.raises(ValueError, match="fit"):
        timer.regs.ARR = 1 << 32
    with pytest.raises(AttributeError):
        _ = timer.regs.NOPE
    with pytest.raises(AttributeError):
        _ = timer.fields.CR.NOPE
    assert timer.now == VirtualTime(m.time.ps)
    assert timer.peripheral == "TIMER"
    assert timer.peer("UNMODELLED") is None
    assert timer.part.part == "POPCORN-M33-TEST"


def test_peer_reaches_another_python_model() -> None:
    m = machine({"TIMER": Timer, "EEPROM": Eeprom})
    assert timer_of(m).peer("EEPROM") is m.model("EEPROM")
    assert timer_of(m).peer("UNMODELLED") is None


def test_a_model_requests_a_stop() -> None:
    class Stopper(Timer):
        def _expire(self) -> None:
            self.request_stop(popcorn.StopReason.USER, "timer expired")

    m = machine({"TIMER": Stopper})
    call(m, "start_timer", 10)
    stop = asyncio.run(m.run(for_=us(100)))
    assert (stop.reason, stop.detail) == (popcorn.StopReason.USER, "timer expired")


def test_a_model_cannot_stop_with_fault() -> None:
    class Faulting(Timer):
        def _expire(self) -> None:
            self.request_stop(popcorn.StopReason.FAULT)

    m = machine({"TIMER": Faulting})
    call(m, "start_timer", 10)
    with pytest.raises(ValueError, match="cannot stop with FAULT"):
        asyncio.run(m.run(for_=us(100)))


def test_a_snapshot_may_return_a_bytearray() -> None:
    class Loose(Timer):
        def snapshot(self) -> bytes:
            return bytearray(super().snapshot())  # pyright: ignore[reportReturnType]

    m = machine({"TIMER": Loose})
    saved = m.snapshot()
    timer_of(m).expirations = 3
    m.restore(saved)
    assert timer_of(m).expirations == 0


def test_a_snapshot_that_is_not_bytes_is_a_model_error() -> None:
    class Wrong(Timer):
        def snapshot(self) -> bytes:
            if self.expirations:
                return 5  # pyright: ignore[reportReturnType]
            return super().snapshot()

    m = machine({"TIMER": Wrong})
    timer_of(m).expirations = 1
    with pytest.raises(popcorn.ModelError, match="not bytes"):
        m.snapshot()


def test_a_failed_model_restore_leaves_the_machine_unusable() -> None:
    class Brittle(Timer):
        def restore(self, data: bytes) -> None:
            if self.expirations == 9:
                raise ValueError("cannot restore")
            super().restore(data)

    m = machine({"TIMER": Brittle})
    saved = m.snapshot()
    timer_of(m).expirations = 9
    with pytest.raises(ValueError, match="cannot restore"):
        m.restore(saved)
    with pytest.raises(popcorn.EmulationError, match="left invalid by a failed restore"):
        asyncio.run(m.run(for_=us(1)))


def test_the_project_wires_a_model_with_its_parameters() -> None:
    project = model_project({"TIMER": popcorn.Periph(model=popcorn.Use(FakeTimer, prescaler=4))})
    model = project.machine().model("TIMER")
    assert model is not None and vars(model)["prescaler"] == 4


@pytest.mark.parametrize(
    ("periph", "key", "expected"),
    [
        (
            popcorn.Periph(model=popcorn.Use(FakeTimer, period=4)),
            "board.TIMER.period",
            "a parameter of Timer",
        ),
        (
            popcorn.Periph(model=popcorn.Use(Plain)),
            "board.TIMER.model",
            "a popcorn.model.Model subclass",
        ),
        (
            popcorn.Periph(devices=[popcorn.Device(FakeEeprom, address=0x50)]),
            "board.TIMER.devices[0].size",
            "a value for Eeprom(size)",
        ),
        (
            popcorn.Periph(devices=[popcorn.Device(FakeEeprom, address=0x50, size=8, speed=1)]),
            "board.TIMER.devices[0].speed",
            "a parameter of Eeprom",
        ),
    ],
)
def test_board_parameters_are_checked_against_the_constructor(
    periph: popcorn.Periph, key: str, expected: str
) -> None:
    project = model_project({"TIMER": periph})
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == key
    assert caught.value.expected.startswith(expected)


def test_a_board_model_needs_a_peripheral_of_the_description() -> None:
    project = model_project({"TIM9": popcorn.Periph(model=popcorn.Use(FakeTimer))})
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == "board.TIM9"


class Stamp(Model):
    """Records the virtual time its event fires."""

    def __init__(self) -> None:
        super().__init__()
        self.fired: list[VirtualTime] = []

    def fire(self) -> None:
        self.fired.append(self.now)


MSIRGSEL = 1 << 23
MSIKRANGE_4MHZ = 4 << 24
MSISRANGE_48MHZ = 0 << 28
MSIS_48_MHZ = MSIRGSEL | MSIKRANGE_4MHZ | MSISRANGE_48MHZ


def test_an_event_keeps_its_virtual_time_across_a_core_clock_change(
    u575: popcorn.PartDescription,
) -> None:
    from .conftest import BOARD_ELF

    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), models={"CRC": Stamp})
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    stamp = m.model("CRC")
    assert isinstance(stamp, Stamp)
    start, counted = m.time, m.instructions
    stamp.schedule(us(100), stamp.fire)
    m.periph.RCC.RCC_ICSCR1.write(MSIS_48_MHZ)
    asyncio.run(m.run(for_=us(200)))
    assert len(stamp.fired) == 1
    late = stamp.fired[0].ps - start.ps - 100_000_000
    assert 0 <= late < 1_000_000
    assert m.instructions - counted > 48 * 200 * 9 // 10


class Picky(Model):
    """Refuses a CR value above 1: a warning, or a ModelError under strict_peripherals."""

    def __init__(self) -> None:
        super().__init__()
        self.strict_at_build = self.strict_peripherals

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        if new <= 1:
            return
        message = f"CR written with {new}, only 0 and 1 are defined"
        if self.strict_peripherals:
            raise popcorn.ModelError(message, peripheral=self.peripheral, register="CR")
        warnings.warn(message, stacklevel=1)


def picky_of(m: popcorn.Machine) -> Picky:
    model = m.model("TIMER")
    assert isinstance(model, Picky)
    return model


@pytest.mark.parametrize("strict", [False, True])
def test_a_model_sees_whether_the_machine_runs_strict(strict: bool) -> None:
    m = machine({"TIMER": Picky}, strict_peripherals=strict)
    picky = picky_of(m)
    assert picky.strict_peripherals is strict
    assert picky.strict_at_build is strict
    saved = m.snapshot()
    call(m, "write_timer_cr", 1)
    m.restore(saved)
    assert picky.strict_peripherals is strict


def test_a_model_warns_unless_the_machine_runs_strict() -> None:
    m = machine({"TIMER": Picky})
    with pytest.warns(UserWarning, match="CR written with 5"):
        call(m, "write_timer_cr", 5)
    assert m.periph.TIMER.CR.value == 5


def test_a_model_raises_a_model_error_under_strict_peripherals() -> None:
    m = machine({"TIMER": Picky}, strict_peripherals=True)
    with pytest.raises(popcorn.ModelError, match="CR written with 5") as caught:
        call(m, "write_timer_cr", 5)
    assert (caught.value.peripheral, caught.value.register) == ("TIMER", "CR")


@pytest.mark.parametrize("strict", [False, True])
def test_strict_peripherals_stays_the_machines_own_across_a_loaded_snapshot(
    strict: bool, tmp_path: Path
) -> None:
    saved = tmp_path / "picky.snap"
    machine({"TIMER": Picky}, strict_peripherals=not strict).snapshot().save(saved)
    m = machine({"TIMER": Picky}, strict_peripherals=strict)
    m.restore(popcorn.Snapshot.load(saved))
    assert picky_of(m).strict_peripherals is strict


_CONTEXT = ("r0", "r1", "r2", "r3", "r4", "r12", "sp", "lr", "pc", "xpsr")


def context_of(m: popcorn.Machine) -> dict[str, int]:
    return {name: int(getattr(m.reg, name)) for name in _CONTEXT}


@pytest.mark.parametrize(
    ("models", "fn"),
    [({"TIMER": Picky}, "write_timer_cr"), ({}, "touch_unmodelled")],
)
def test_a_model_error_inside_a_call_restores_the_interrupted_context(
    models: Mapping[str, Callable[[], Model]], fn: str
) -> None:
    m = machine(models, strict_peripherals=True)
    before = context_of(m)
    with pytest.raises(popcorn.ModelError):
        asyncio.run(m.call(fn, *([5] if fn == "write_timer_cr" else []), budget=10_000))
    assert context_of(m) == before
    assert not m.busy
    stop = asyncio.run(m.step())
    assert stop.reason is popcorn.StopReason.BUDGET
    assert m.pc != before["pc"]


class ExpiryFails(Timer):
    def _expire(self) -> None:
        raise LookupError("expiry failed")


class StatusWriteFails(Timer):
    @on_write("SR")
    def _sr(self, old: int, new: int) -> None:
        raise LookupError("SR write failed")


@pytest.mark.parametrize("failing", [ExpiryFails, StatusWriteFails])
def test_a_model_error_inside_running_raises_at_the_next_wait(failing: type[Timer]) -> None:
    m = machine({"TIMER": failing})
    call(m, "start_timer", 10)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("stop_timer"), for_=ms(100))

    with pytest.raises(LookupError, match="failed"):
        run_bounded(scenario())
    assert not m.busy
