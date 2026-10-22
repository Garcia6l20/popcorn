"""RCC peripheral resets reaching a Python model: `Model.on_bus_reset` and `held_in_reset`."""

from __future__ import annotations

import asyncio
import gc
import weakref
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn.model import Model, on_read, on_write
from popcorn.units import us

from .conftest import run_bounded

RNG_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rng_ll.elf"
RNGRST = 1 << 18
HASHRST = 1 << 17
DEINIT_LOGGED = 31


class Rng(Model):
    """Records CR accesses, bus resets and machine resets of the RNG."""

    def __init__(self) -> None:
        super().__init__()
        self.writes: list[int] = []
        self.reads = 0
        self.bus_resets = 0
        self.resets: list[popcorn.ResetKind] = []

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        self.writes.append(new)

    @on_read("CR")
    def _cr_read(self) -> int:
        self.reads += 1
        return self.regs.CR

    def on_bus_reset(self) -> None:
        self.bus_resets += 1
        super().on_bus_reset()

    def reset(self, kind: popcorn.ResetKind) -> None:
        self.resets.append(kind)
        super().reset(kind)


class Every(Model):
    """Uses every hook the native bridge looks up: claims, reset, bus reset, read, write, event,
    save and load."""

    ticks: int = 0

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        self.schedule(us(1), self._tick)

    @on_read("DR")
    def _dr(self) -> int:
        return self.ticks

    def _tick(self) -> None:
        self.ticks += 1


class Raising(Model):
    """Raises from its bus reset."""

    @on_write("IMR")
    def _imr(self, old: int, new: int) -> None:
        pass

    def on_bus_reset(self) -> None:
        raise RuntimeError("bus reset refused")


def entered(
    u575: popcorn.PartDescription, entry: str = "idle", model: type[Model] = Rng
) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RNG_ELF), models={"RNG": model})
    m.reset()
    assert asyncio.run(m.run(until="main", budget=100_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym(entry)
    return m


def rng_of(m: popcorn.Machine) -> Rng:
    model = m.model("RNG")
    assert isinstance(model, Rng)
    return model


def test_a_pulse_calls_on_bus_reset_once_and_reset_with_the_peripheral_kind(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575)
    model = rng_of(m)
    model.resets.clear()
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    assert m.periph.RNG.held_in_reset
    m.periph.RCC.AHB2RSTR1.write(0)
    assert not m.periph.RNG.held_in_reset
    assert model.bus_resets == 1
    assert model.resets == [popcorn.ResetKind.PERIPHERAL]


def test_a_machine_reset_refuses_the_peripheral_kind(u575: popcorn.PartDescription) -> None:
    m = entered(u575)
    with pytest.raises(popcorn.ConfigError):
        m.reset(popcorn.ResetKind.PERIPHERAL)


def test_writes_are_ignored_while_held_and_reach_the_model_after(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575)
    model = rng_of(m)
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RNG.CR.write(0x4)
    assert model.writes == []
    assert m.periph.RNG.CR.value & 0x4 == 0
    m.periph.RCC.AHB2RSTR1.write(0)
    m.periph.RNG.CR.write(0x4)
    assert model.writes == [0x4]


def test_a_claimed_register_returns_to_its_reset_value_on_a_bus_and_a_machine_reset(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575)
    reset_value = m.periph.RNG.CR.value
    m.periph.RNG.CR.write(0x4)
    assert m.periph.RNG.CR.value != reset_value
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RCC.AHB2RSTR1.write(0)
    assert m.periph.RNG.CR.value == reset_value
    m.periph.RNG.CR.write(0x4)
    m.reset()
    assert m.periph.RNG.CR.value == reset_value


def test_a_model_using_every_hook_runs_them_all_in_one_machine_life(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575, model=Every)
    m.periph.RNG.CR.write(0x4)
    asyncio.run(m.run(for_=us(5), budget=100_000))
    assert m.periph.RNG.DR.read() == 1
    saved = m.snapshot()
    m.restore(saved)
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RCC.AHB2RSTR1.write(0)
    m.reset()
    assert m.periph.RNG.DR.read() == 0


def test_a_raising_bus_reset_still_holds_every_instance_of_the_write(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RNG_ELF), models={"HASH": Raising})
    m.reset()
    names = [p.name for p in u575.peripherals]
    assert names.index("HASH") < names.index("RNG")
    with pytest.raises(RuntimeError, match="bus reset refused"):
        m.periph.RCC.AHB2RSTR1.write(RNGRST | HASHRST)
    assert m.periph.HASH.held_in_reset
    assert m.periph.RNG.held_in_reset
    before = m.periph.RNG.CR.value
    m.periph.RNG.CR.write(before | 0x4)
    assert m.periph.RNG.CR.value == before


def deinit_parked(m: popcorn.Machine) -> None:
    stop = asyncio.run(m.run(budget=2_000_000))
    assert stop.reason in (popcorn.StopReason.USER, popcorn.StopReason.IDLE), stop


def deinit_running(m: popcorn.Machine) -> None:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("log_len").ge(DEINIT_LOGGED))

    model = rng_of(m)
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    calls = model.bus_resets + len(model.writes) + model.reads
    assert m._handle.gil_entries() - before == calls  # pyright: ignore[reportPrivateUsage]


@pytest.mark.parametrize("run", [deinit_parked, deinit_running])
def test_ll_rng_deinit_resets_the_python_model_once_parked_and_running(
    u575: popcorn.PartDescription, run: Callable[[popcorn.Machine], None]
) -> None:
    m = entered(u575, "deinit")
    model = rng_of(m)
    run(m)
    assert model.bus_resets == 1
    assert not m.periph.RNG.held_in_reset


def test_a_reset_model_is_collected_with_its_machine(u575: popcorn.PartDescription) -> None:
    m = entered(u575)
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RCC.AHB2RSTR1.write(0)
    model = weakref.ref(rng_of(m))
    machine = weakref.ref(m)
    del m
    gc.collect()
    assert model() is None
    assert machine() is None
