import struct
from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import FaultKind, StopReason
from popcorn.model import Model, on_read
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

IBUSERR = 1 << 8
HFSR = 0xE000_ED2C
FORCED = 1 << 30
HARDFAULT = 3

# --8<-- [start:descriptions]
STOPS = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)
HANDLES = STOPS.replace(faults="handler")
# --8<-- [end:descriptions]


@popcorn.pytest.machine
def stops() -> popcorn.MachineDescription:
    return STOPS


@popcorn.pytest.machine
def handles() -> popcorn.MachineDescription:
    return HANDLES


async def test_a_jump_to_address_0_is_a_fault_stop(stops: popcorn.Machine) -> None:
    # --8<-- [start:stop]
    stops.reg.pc = 0
    stop = await stops.run(for_=ms(1))
    assert stop.reason is StopReason.FAULT
    assert stop.fault is not None
    assert stop.fault.kind is FaultKind.BUS
    assert stop.fault.address == 0
    assert stop.fault.cfsr & IBUSERR
    # --8<-- [end:stop]


async def test_with_handlers_the_firmware_takes_the_fault(handles: popcorn.Machine) -> None:
    # --8<-- [start:handler]
    handles.reg.pc = 0
    stop = await handles.run(until="HardFault_Handler", for_=ms(1))
    assert stop.reason is StopReason.ADDRESS
    assert stop.pc == handles.sym("HardFault_Handler")
    assert handles.reg.xpsr & 0x1FF == HARDFAULT
    assert handles.mem.u32[HFSR] & FORCED
    # --8<-- [end:handler]


def test_a_host_read_at_address_0_raises(stops: popcorn.Machine) -> None:
    # --8<-- [start:host]
    with pytest.raises(popcorn.BusAccessError) as refused:
        stops.mem.u32[0]
    assert not refused.value.denied
    # --8<-- [end:host]


async def test_unmodelled_data_is_reachable_through_the_storage(stops: popcorn.Machine) -> None:
    # --8<-- [start:unmodelled]
    cr = stops.periph.WWDG.CR
    with pytest.raises(popcorn.ModelError):
        cr.read()
    cr.poke(0x3F)
    assert cr.value == 0x3F
    with pytest.raises(popcorn.ModelError):
        stops.mem.u32[0xE004_2000]
    # --8<-- [end:unmodelled]


PRECISERR = 1 << 9
IMPRECISERR = 1 << 10
BFARVALID = 1 << 15
UNALIGNED = 1 << 24
CODE = 0x2000_0100
LOAD = struct.pack("<HHH", 0xF8D1, 0x0000, 0xE7FE)
LDREX = struct.pack("<HHH", 0xE851, 0x0F00, 0xE7FE)
TRUSTZONE = Path(__file__).parents[3] / "examples/stm32u575/trustzone/prebuilt"


class Failing(Model):
    @on_read("DR")
    def _dr(self) -> int | None:
        return None


FAILING = STOPS.replace(board={"RNG": popcorn.Periph(model=popcorn.Use(Failing))})
SECURE = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[
        popcorn.Elf(TRUSTZONE / "trustzone.elf", name="nonsecure"),
        popcorn.Elf(TRUSTZONE / "trustzone_s.elf", name="secure", loaded=False),
    ],
    option_bytes=popcorn.OptionBytes(tzen=True, secwm1=(0, 3), secwm2=None),
    boot=popcorn.Boot(until="main", image="nonsecure", budget=100_000),
)


@popcorn.pytest.machine
def failing() -> popcorn.MachineDescription:
    return FAILING


@popcorn.pytest.machine
def secure() -> popcorn.MachineDescription:
    return SECURE


async def run_code(machine: popcorn.Machine, code: bytes, r1: int) -> popcorn.Stop:
    machine.mem.write(CODE, code)
    machine.reg.pc = CODE
    machine.reg.r1 = r1
    return await machine.run(for_=ms(1))


async def test_an_unaligned_ldrex_is_a_usagefault(stops: popcorn.Machine) -> None:
    stop = await run_code(stops, LDREX, 0x2000_0001)
    assert stop.reason is StopReason.FAULT
    assert stop.fault is not None
    assert stop.fault.kind is FaultKind.USAGE
    assert stop.fault.cfsr & UNALIGNED


async def test_a_load_at_address_0_is_a_precise_busfault(stops: popcorn.Machine) -> None:
    stop = await run_code(stops, LOAD, 0)
    assert stop.fault is not None
    assert stop.fault.kind is FaultKind.BUS
    assert stop.fault.cfsr & PRECISERR
    assert stop.fault.cfsr & BFARVALID
    assert stop.fault.address == 0


async def test_a_bus_error_of_a_model_is_imprecise(failing: popcorn.Machine) -> None:
    stop = await run_code(failing, LOAD, 0x420C_0808)
    assert stop.fault is not None
    assert stop.fault.kind is FaultKind.BUS
    assert stop.fault.cfsr & IMPRECISERR
    assert not stop.fault.cfsr & (PRECISERR | BFARVALID)


async def test_a_firewall_refusal_is_a_precise_busfault(secure: popcorn.Machine) -> None:
    stop = await run_code(secure, LOAD, 0x4603_2400)
    assert stop.fault is not None
    assert stop.fault.kind is FaultKind.BUS
    assert stop.fault.cfsr & PRECISERR
    assert stop.fault.cfsr & BFARVALID
    assert stop.fault.address == 0x4603_2400
