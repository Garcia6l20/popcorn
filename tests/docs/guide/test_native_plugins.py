from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import ConfigError, StopReason
from popcorn.plugin import Plugin
from popcorn.units import ms

EXAMPLE = Path(__file__).parents[3] / "examples/stm32u575/native_plugin"

# --8<-- [start:describe]
PLUGIN = Plugin.dev(EXAMPLE / "plugin")

NATIVE = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(EXAMPLE / "prebuilt/native_plugin.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    plugins=[PLUGIN],
    board={"RNG": popcorn.Periph(model=PLUGIN.model("rng"))},
)


@popcorn.pytest.machine
def native() -> popcorn.MachineDescription:
    return NATIVE


# --8<-- [end:describe]


def xorshift32(seed: int) -> int:
    seed ^= seed << 13 & 0xFFFFFFFF
    seed ^= seed >> 17
    seed ^= seed << 5 & 0xFFFFFFFF
    return seed


async def test_the_firmware_reads_the_words_of_the_native_model(native: popcorn.Machine) -> None:
    # --8<-- [start:run]
    stop = await native.run(until="rng_done", for_=ms(20))
    assert stop.reason is StopReason.ADDRESS
    result = native.var("rng_result")
    words = [int(result.words[i].value) for i in range(8)]
    expected = [xorshift32(1)]
    for _ in range(7):
        expected.append(xorshift32(expected[-1]))
    assert words == expected
    # --8<-- [end:run]


def test_an_unknown_model_name_is_refused() -> None:
    # --8<-- [start:unknown]
    broken = NATIVE.replace(board={"RNG": popcorn.Periph(model=PLUGIN.model("nothing"))})
    with pytest.raises(ConfigError):
        broken.machine()
    # --8<-- [end:unknown]
