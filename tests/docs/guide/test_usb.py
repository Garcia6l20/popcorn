import math
from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.board import UsbHostOptions, UsbState
from popcorn.units import ms, s

EXAMPLES = Path(__file__).parents[3] / "examples/stm32u575"

# --8<-- [start:describe]
CDC = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(EXAMPLES / "usb_cdc/prebuilt/usb_cdc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={"OTG_FS": popcorn.Periph(host=UsbHostOptions())},
)
HEADSET = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(EXAMPLES / "usb_audio/prebuilt/usb_audio.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={"OTG_FS": popcorn.Periph(host=UsbHostOptions())},
)


@popcorn.pytest.machine
def cdc() -> popcorn.MachineDescription:
    return CDC


@popcorn.pytest.machine
def headset() -> popcorn.MachineDescription:
    return HEADSET


# --8<-- [end:describe]

pytestmark = pytest.mark.popcorn(budget=5_000_000)


async def test_the_host_enumerates_the_device(cdc: popcorn.Machine) -> None:
    # --8<-- [start:enumerate]
    host = cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    assert host.state is UsbState.CONFIGURED
    assert host.address == 1
    assert host.strings[1] == "TinyUSB"
    assert [interface.number for interface in host.interfaces] == [0, 1, 2, 3]
    # --8<-- [end:enumerate]


async def test_the_cdc_function_echoes(cdc: popcorn.Machine) -> None:
    # --8<-- [start:cdc]
    function = cdc.periph.OTG_FS.host.cdc()
    await function.write(b"hello")
    assert await function.read(5, for_=s(2)) == b"HELLO"
    # --8<-- [end:cdc]


async def test_a_press_sends_a_hid_report(cdc: popcorn.Machine) -> None:
    # --8<-- [start:hid]
    host = cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    hid = host.hid(0)
    await hid.wait_ready(for_=s(2))
    pin = cdc.periph.GPIOC.pin(13)
    pin.level = 0
    pin.level = 1
    [report] = await hid.read_reports(1, for_=ms(50))
    assert report.data == b"\x01\x01"
    # --8<-- [end:hid]


async def test_unplugging_enumerates_again(cdc: popcorn.Machine) -> None:
    # --8<-- [start:detach]
    host = cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await host.detach()
    await cdc.run(for_=ms(5))
    assert host.state is UsbState.DETACHED

    await host.attach()
    await host.wait_configured(for_=s(1))
    assert host.state is UsbState.CONFIGURED
    # --8<-- [end:detach]


async def test_a_sine_comes_back_on_the_microphone(headset: popcorn.Machine) -> None:
    # --8<-- [start:audio]
    host = headset.periph.OTG_FS.host
    audio = host.audio()
    await host.wait_configured(for_=s(2))
    await audio.wait_ready(for_=ms(200))
    await audio.mic.start()
    await audio.speaker.start()
    await headset.run(for_=ms(4))
    audio.mic.clear()

    sine = [round(12000 * math.sin(2 * math.pi * 1000 * n / 16000)) for n in range(16 * 160)]
    await audio.speaker.write(sine)
    frames = await audio.mic.read_frames(170, for_=ms(200))
    heard = [sample for frame in frames for sample in frame.samples]
    start = next(n for n, sample in enumerate(heard) if sample) - 1
    assert heard[start : start + len(sine)] == sine
    # --8<-- [end:audio]
