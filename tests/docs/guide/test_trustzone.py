from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import BusAccessError, StopReason
from popcorn.units import ms

PREBUILT = Path(__file__).parents[3] / "examples/stm32u575/trustzone/prebuilt"

# --8<-- [start:describe]
SECURE = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[
        popcorn.Elf(PREBUILT / "trustzone.elf", name="nonsecure"),
        popcorn.Elf(PREBUILT / "trustzone_s.elf", name="secure", loaded=False),
    ],
    option_bytes=popcorn.OptionBytes(tzen=True, secwm1=(0, 3), secwm2=None),
    boot=popcorn.Boot(until="main", image="nonsecure", budget=100_000),
    faults="handler",
)


@popcorn.pytest.machine
def trustzone() -> popcorn.MachineDescription:
    return SECURE


# --8<-- [end:describe]


async def test_the_views_follow_the_security_state(trustzone: popcorn.Machine) -> None:
    # --8<-- [start:views]
    m = trustzone
    assert m.security_state == "nonsecure"
    assert m.reg.msp == m.reg.msp_ns
    m.mem.u32[0x2002_FE00] = 0x1234
    assert m.mem.secure.u32[0x3002_FE00] == 0x1234
    assert m.mem.secure.u32[0x2002_FE00] == 0
    with pytest.raises(BusAccessError) as denied:
        m.mem.nonsecure.u32[0x3002_FE00]
    assert denied.value.denied
    # --8<-- [end:views]


async def test_a_probe_counts_in_one_state(trustzone: popcorn.Machine) -> None:
    # --8<-- [start:probe]
    veneer = trustzone.probe("secure_counter_add", state="nonsecure")
    function = trustzone.sym("__acle_se_secure_counter_add", image="secure")
    behind = trustzone.probe(function, state="secure")
    stop = await trustzone.run(for_=ms(5))
    assert stop.reason is StopReason.TIME
    assert veneer.count >= 1
    assert behind.count == veneer.count
    # --8<-- [end:probe]
