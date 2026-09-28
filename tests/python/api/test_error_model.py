"""NFR-ERR-1: every failing public operation raises a PopcornError subclass with its context."""

from __future__ import annotations

import asyncio
import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import popcorn
import pytest

from .conftest import DRIVER_ELF, M33_JSON, ROOT, Machines
from .models_example import Timer

MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"
ENGINE_TEXT = re.compile(r"\bUC_ERR|\buc_[a-z_]+\(|unicorn", re.IGNORECASE)


@dataclass(frozen=True, slots=True)
class Case:
    name: str
    call: Callable[[Path, Machines], object]
    error: type[popcorn.PopcornError]
    context: Callable[[popcorn.PopcornError], bool]


def machine() -> popcorn.Machine:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(DRIVER_ELF))
    m.reset()
    return m


def project(tmp_path: Path, text: str) -> popcorn.Project:
    path = tmp_path / "popcorn.toml"
    path.write_text(text)
    return popcorn.Project.load(path)


def minimal(extra: str = "") -> str:
    return (
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{DRIVER_ELF}"\n[boot]\nuntil = "alu_mark"\n{extra}'
    )


def missing_description(tmp_path: Path, _: Machines) -> object:
    return popcorn.load_description(tmp_path / "absent.json")


def bad_elf(tmp_path: Path, _: Machines) -> object:
    path = tmp_path / "bad.elf"
    path.write_bytes(b"not an elf")
    return popcorn.Image.elf(path)


def unknown_symbol(_: Path, __: Machines) -> object:
    return machine().sym("alu_mrak")


def unmapped_read(_: Path, __: Machines) -> object:
    return machine().mem.read(0x6000_0000, 4)


def wide_register(_: Path, __: Machines) -> object:
    machine().reg.r0 = 1 << 40
    return None


def foreign_snapshot(_: Path, __: Machines) -> object:
    machine().restore(machine().snapshot())
    return None


def unknown_model_peripheral(_: Path, __: Machines) -> object:
    return popcorn.Machine(M33_JSON, models={"TIMR": Timer})


def strict_unmodelled(_: Path, __: Machines) -> object:
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        strict_peripherals=True,
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    return asyncio.run(m.call("touch_unmodelled"))


def fault_in_running(_: Path, machines: Machines) -> object:
    m = machines("fault")

    async def body() -> None:
        async with m.running():
            await asyncio.sleep(0.05)

    return asyncio.run(body())


def boot_budget(tmp_path: Path, _: Machines) -> object:
    loaded = project(tmp_path, minimal("budget = 100\n"))
    return asyncio.run(loaded.boot(loaded.machine()))


def unknown_project_key(tmp_path: Path, _: Machines) -> object:
    return project(tmp_path, minimal("colour = 3\n"))


def garbage_snapshot(tmp_path: Path, _: Machines) -> object:
    path = tmp_path / "bad.snap"
    path.write_bytes(b"garbage")
    return machine().load_snapshot(path)


def state_while_running(_: Path, machines: Machines) -> object:
    m = machines()

    async def body() -> int:
        async with m.running():
            return m.pc

    return asyncio.run(body())


def second_run(_: Path, machines: Machines) -> object:
    m = machines()

    async def body() -> None:
        first = asyncio.create_task(m.run(budget=10_000_000))
        await asyncio.sleep(0)
        try:
            await m.run(budget=1)
        finally:
            await first

    return asyncio.run(body())


def _config(key: str) -> Callable[[popcorn.PopcornError], bool]:
    return lambda e: isinstance(e, popcorn.ConfigError) and key in e.key


CASES = [
    Case("missing description", missing_description, popcorn.ConfigError, lambda e: bool(e.args)),
    Case(
        "bad ELF",
        bad_elf,
        popcorn.ImageError,
        lambda e: isinstance(e, popcorn.ImageError) and e.segment == "header",
    ),
    Case(
        "unknown symbol",
        unknown_symbol,
        popcorn.SymbolError,
        lambda e: isinstance(e, popcorn.SymbolError) and "alu_mark" in e.close_matches,
    ),
    Case(
        "unmapped read",
        unmapped_read,
        popcorn.EmulationError,
        lambda e: isinstance(e, popcorn.EmulationError) and e.address == 0x6000_0000,
    ),
    Case("wide register", wide_register, popcorn.ConfigError, _config("r0")),
    Case("foreign snapshot", foreign_snapshot, popcorn.ConfigError, _config("machine")),
    Case("model peripheral", unknown_model_peripheral, popcorn.ConfigError, _config("TIMR")),
    Case(
        "strict unmodelled",
        strict_unmodelled,
        popcorn.ModelError,
        lambda e: isinstance(e, popcorn.ModelError)
        and (e.peripheral, e.register) == ("UNMODELLED", "DATA"),
    ),
    Case(
        "fault",
        fault_in_running,
        popcorn.FaultError,
        lambda e: isinstance(e, popcorn.FaultError) and e.pc is not None and e.symbol != "",
    ),
    Case(
        "budget",
        boot_budget,
        popcorn.BudgetExceeded,
        lambda e: isinstance(e, popcorn.BudgetExceeded) and e.budget == 100,
    ),
    Case(
        "project key",
        unknown_project_key,
        popcorn.ConfigError,
        lambda e: isinstance(e, popcorn.ConfigError) and e.key == "boot.colour" and e.line > 0,
    ),
    Case("garbage snapshot", garbage_snapshot, popcorn.ConfigError, lambda e: bool(e.args)),
    Case("state while running", state_while_running, popcorn.MachineRunning, lambda e: True),
    Case("second run", second_run, popcorn.MachineBusy, lambda e: True),
]


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_failures_are_typed_with_context(case: Case, tmp_path: Path, machines: Machines) -> None:
    with pytest.raises(popcorn.PopcornError) as caught:
        case.call(tmp_path, machines)
    error = caught.value
    assert type(error) is case.error, repr(error)
    assert case.context(error), repr(vars(error))
    assert not ENGINE_TEXT.search(str(error)), str(error)


def test_every_error_class_is_covered() -> None:
    covered = {case.error for case in CASES}
    public = {
        cls
        for cls in vars(popcorn).values()
        if isinstance(cls, type)
        and issubclass(cls, popcorn.PopcornError)
        and cls is not popcorn.PopcornError
    }
    assert public <= covered | {popcorn.EmulationError}
    assert popcorn.EmulationError in covered
