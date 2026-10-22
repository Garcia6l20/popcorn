from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import FaultKind, StopReason
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
    with pytest.raises(popcorn.BusAccessError):
        stops.mem.u32[0]
    # --8<-- [end:host]
