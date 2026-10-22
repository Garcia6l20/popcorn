"""RCC peripheral resets reaching a Python model: `Model.on_bus_reset` and `held_in_reset`."""

from __future__ import annotations

import asyncio
import gc
import weakref
from pathlib import Path

import popcorn
import pytest
from popcorn.model import Model, on_write

from .conftest import run_bounded

RNG_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rng_ll.elf"
RNGRST = 1 << 18
DEINIT_LOGGED = 31


class Rng(Model):
    """Records CR writes, bus resets and machine resets of the RNG."""

    def __init__(self) -> None:
        super().__init__()
        self.writes: list[int] = []
        self.bus_resets = 0
        self.resets: list[popcorn.ResetKind] = []

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        self.writes.append(new)

    def on_bus_reset(self) -> None:
        self.bus_resets += 1
        super().on_bus_reset()

    def reset(self, kind: popcorn.ResetKind) -> None:
        self.resets.append(kind)
        super().reset(kind)


def entered(u575: popcorn.PartDescription, entry: str = "idle") -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RNG_ELF), models={"RNG": Rng})
    m.reset()
    assert asyncio.run(m.run(until="main", budget=100_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym(entry)
    return m


def rng_of(m: popcorn.Machine) -> Rng:
    model = m.model("RNG")
    assert isinstance(model, Rng)
    return model


def test_a_pulse_calls_on_bus_reset_once_and_reset_with_the_software_kind(
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
    assert model.resets == [popcorn.ResetKind.SOFTWARE]


def test_writes_are_ignored_while_held_and_reach_the_model_after(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575)
    model = rng_of(m)
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RNG.CR.write(0x4)
    assert model.writes == []
    assert m.periph.RNG.CR.value == m.periph.RNG.CR.value & ~0x4
    m.periph.RCC.AHB2RSTR1.write(0)
    m.periph.RNG.CR.write(0x4)
    assert model.writes == [0x4]


def deinit_parked(m: popcorn.Machine) -> None:
    stop = asyncio.run(m.run(budget=2_000_000))
    assert stop.reason in (popcorn.StopReason.USER, popcorn.StopReason.IDLE), stop


def deinit_running(m: popcorn.Machine) -> None:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("log_len").ge(DEINIT_LOGGED))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER


@pytest.mark.parametrize("run", [deinit_parked, deinit_running])
def test_ll_rng_deinit_resets_the_python_model_once_parked_and_running(
    u575: popcorn.PartDescription, run: object
) -> None:
    m = entered(u575, "deinit")
    model = rng_of(m)
    assert callable(run)
    run(m)
    assert model.bus_resets == 1
    assert not m.periph.RNG.held_in_reset


def test_a_reset_model_is_collected_with_its_machine(u575: popcorn.PartDescription) -> None:
    m = entered(u575)
    m.periph.RCC.AHB2RSTR1.write(RNGRST)
    m.periph.RCC.AHB2RSTR1.write(0)
    ref = weakref.ref(rng_of(m))
    del m
    gc.collect()
    assert ref() is None
