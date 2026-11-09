import json
import random
from pathlib import Path
from typing import ClassVar, override

import popcorn
import pytest
from popcorn import ModelError, SnapshotRefused, StopReason
from popcorn.board import (
    AudioFrame,
    DeviceAudio,
    LineFormat,
    RccOptions,
    UartDevice,
    UartError,
)
from popcorn.stm32 import mcu
from popcorn.units import VirtualTime, ms, us

ROOT = Path(__file__).parents[3]
AT_CLIENT = ROOT / "examples/stm32u575/uart_mock/prebuilt/at_client.elf"
BUTTON = ROOT / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"
SAI_ELF = ROOT / "tests/firmware/periph/prebuilt/sai_ll.elf"
PS_PER_SECOND = 10**12


# --8<-- [start:modem]
class AtModem(UartDevice):
    replies: ClassVar[dict[bytes, bytes]] = {
        b"AT": b"OK\r\n",
        b"AT+CSQ": b"+CSQ: 17,99\r\n\r\nOK\r\n",
        b"AT+SEND=hello": b"OK\r\n",
    }
    delay = 0.005

    def __init__(self, *, line: LineFormat | None = None) -> None:
        super().__init__(line=line)
        self.buffer = bytearray()
        self.commands: list[tuple[bytes, VirtualTime]] = []
        self.errors: list[tuple[UartError, VirtualTime]] = []

    @override
    def on_attach(self) -> None:
        self.buffer = bytearray()
        self.commands = []
        self.errors = []

    @override
    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.buffer.append(byte)
        if self.buffer.endswith(b"\r\n"):
            command = bytes(self.buffer[:-2])
            self.buffer.clear()
            self.commands.append((command, at))
            self.answer(command)

    def answer(self, command: bytes) -> None:
        reply = self.replies.get(command, b"ERROR\r\n")
        self.loop.call_later(self.delay, self.send, reply)

    @override
    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.errors.append((kind, at))


# --8<-- [end:modem]


# --8<-- [start:start]
async def start(device: UartDevice) -> popcorn.Machine:
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(AT_CLIENT),
        devices={"USART2": [device]},
    )
    machine.reset()
    await machine.run(until="main", budget=1_000_000)
    return machine


# --8<-- [end:start]


async def finish(machine: popcorn.Machine) -> None:
    stop = await machine.run(until="modem_done", for_=ms(500))
    assert stop.reason is StopReason.ADDRESS, stop


async def test_the_modem_answers_5_ms_after_each_command() -> None:
    # --8<-- [start:run]
    modem = AtModem()
    modem.tapping = True
    machine = await start(modem)
    await finish(machine)

    result = machine.var("modem_result")
    assert (result.at.value, result.csq.value, result.rssi.value) == (1, 1, 17)
    assert [command for command, _ in modem.commands] == list(AtModem.replies)
    # --8<-- [end:run]

    # --8<-- [start:tap]
    _, last_frame = modem.commands[0]
    first_reply = modem.tap_frames("USART2")[0]
    assert first_reply.start == last_frame + ms(5)
    assert [frame.data for frame in modem.tap_frames("device")[:4]] == list(b"AT\r\n")
    # --8<-- [end:tap]


async def test_two_runs_give_the_same_stamps() -> None:
    async def once() -> list[tuple[bytes, VirtualTime]]:
        modem = AtModem()
        await finish(await start(modem))
        return modem.commands

    assert await once() == await once()


async def test_a_device_in_another_baud_hears_framing_errors() -> None:
    # --8<-- [start:baud]
    modem = AtModem(line=LineFormat(baud=230400))
    machine = await start(modem)
    await finish(machine)

    assert UartError.FRAMING in {kind for kind, _ in modem.errors}
    assert machine.var("modem_result").at.value == 3
    # --8<-- [end:baud]


async def test_a_stamped_post_tells_the_modem_to_hang_up() -> None:
    # --8<-- [start:post]
    class Hanging(AtModem):
        hung_up = False

        def hang_up(self) -> None:
            self.hung_up = True

        @override
        def answer(self, command: bytes) -> None:
            if not self.hung_up:
                super().answer(command)

    modem = Hanging()
    machine = await start(modem)
    pending = machine.loop.post(modem.hang_up, at=machine.time + ms(10))
    await finish(machine)
    await pending

    assert machine.var("modem_result").send.value == 3
    # --8<-- [end:post]


async def test_a_seeded_generator_makes_runs_identical() -> None:
    # --8<-- [start:seed]
    class Jittery(AtModem):
        def __init__(self, seed: int) -> None:
            super().__init__()
            self.seed = seed

        @override
        def on_attach(self) -> None:
            super().on_attach()
            self.rng = random.Random(self.seed)

        @override
        def answer(self, command: bytes) -> None:
            reply = self.replies.get(command, b"ERROR\r\n")
            self.loop.call_later(self.rng.uniform(0.001, 0.009), self.send, reply)

    # --8<-- [end:seed]

    async def once(seed: int) -> list[VirtualTime]:
        modem = Jittery(seed)
        modem.tapping = True
        await finish(await start(modem))
        return [frame.start for frame in modem.tap_frames("USART2")]

    assert await once(1234) == await once(1234)
    assert await once(1234) != await once(99)


# --8<-- [start:resumable]
class Resumable(AtModem):
    def __init__(self) -> None:
        super().__init__()
        self.due: list[tuple[int, bytes]] = []

    @override
    def on_attach(self) -> None:
        super().on_attach()
        self.due = []

    @override
    def answer(self, command: bytes) -> None:
        reply = self.replies.get(command, b"ERROR\r\n")
        self.schedule(self.now.ps + round(self.delay * PS_PER_SECOND), reply)

    def schedule(self, due: int, reply: bytes) -> None:
        self.due.append((due, reply))
        self.loop.call_at(due / PS_PER_SECOND, self.deliver, reply)

    def deliver(self, reply: bytes) -> None:
        self.due.pop(0)
        self.send(reply)

    @override
    def snapshot(self) -> bytes:
        due = [(at, list(reply)) for at, reply in self.due]
        return json.dumps({"buffer": list(self.buffer), "due": due}).encode()

    @override
    def restore(self, data: bytes) -> None:
        saved = json.loads(data)
        self.buffer = bytearray(saved["buffer"])
        self.due = []
        self.commands = []
        self.errors = []
        for due, reply in saved["due"]:
            self.schedule(due, bytes(reply))


# --8<-- [end:resumable]


async def test_a_default_snapshot_refuses_after_traffic() -> None:
    # --8<-- [start:refused]
    machine = await start(AtModem())
    await machine.run(for_=ms(3))
    with pytest.raises(SnapshotRefused):
        machine.snapshot()
    # --8<-- [end:refused]


async def test_an_overriding_device_restores_a_stack_with_a_reply_due() -> None:
    # --8<-- [start:resume]
    modem = Resumable()
    machine = await start(modem)
    await machine.run(for_=ms(3))
    snapshot = machine.snapshot()
    assert modem.due

    await finish(machine)
    first = list(modem.commands)

    machine.restore(snapshot)
    assert modem.due
    await finish(machine)
    assert modem.commands == first[1:]
    assert machine.var("modem_result").send.value == 1
    # --8<-- [end:resume]


async def test_a_snapshot_right_after_reset_restores_and_runs_like_no_snapshot() -> None:
    reference = Resumable()
    await finish(await start(reference))

    modem = Resumable()
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(AT_CLIENT),
        devices={"USART2": [modem]},
    )
    machine.reset()
    snapshot = machine.snapshot()
    machine.restore(snapshot)
    await machine.run(until="main", budget=1_000_000)
    await finish(machine)
    assert modem.commands == reference.commands


async def test_a_quiet_stack_restores_by_building_again() -> None:
    modem = AtModem()
    machine = await start(modem)
    snapshot = machine.snapshot()
    await finish(machine)
    first = list(modem.commands)
    machine.restore(snapshot)
    await finish(machine)
    assert modem.commands == first


async def test_a_raising_hook_stops_the_run_and_the_next_run_goes_on() -> None:
    # --8<-- [start:raising]
    class Flaky(AtModem):
        raised = False

        @override
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            super().on_frame(byte, at)
            if len(self.buffer) == 1 and not self.raised:
                self.raised = True
                raise RuntimeError("stack bug")

    modem = Flaky()
    machine = await start(modem)
    with pytest.raises(ModelError, match="stack bug"):
        await machine.run(until="modem_done", for_=ms(500))
    await finish(machine)
    assert machine.var("modem_result").send.value == 1
    # --8<-- [end:raising]


async def test_a_device_watches_and_drives_pins() -> None:
    # --8<-- [start:pins]
    class Presser(UartDevice):
        @override
        def on_attach(self) -> None:
            self.led: list[tuple[bool, VirtualTime]] = []
            self.board.watch_pin("PA5", self.on_led)
            self.board.drive_pin("PC13", False, at=self.now + ms(2))
            self.board.drive_pin("PC13", True, at=self.now + ms(3))

        def on_led(self, level: bool, at: VirtualTime) -> None:
            self.led.append((level, at))

    presser = Presser()
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(BUTTON),
        devices={"USART1": [presser]},
    )
    machine.reset()
    await machine.run(until="main", budget=1_000_000)
    await machine.run(for_=ms(6))

    assert [level for level, _ in presser.led] == [True]
    assert machine.var("presses").value == 1
    # --8<-- [end:pins]


async def test_a_device_reads_the_clock_the_mcu_drives_on_a_pin() -> None:
    # --8<-- [start:pin-clock]
    class LpoChip(UartDevice):
        @override
        def on_attach(self) -> None:
            self.lpo: list[tuple[int, VirtualTime]] = []
            self.board.watch_pin_clock("PA2", self.on_lpo)

        def on_lpo(self, hz: int, at: VirtualTime) -> None:
            self.lpo.append((hz, at))

    chip = LpoChip()
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(BUTTON),
        devices={"USART1": [chip]},
        rcc=RccOptions(lsi_hz=31_550),
    )
    machine.reset()
    await machine.run(until="main", budget=1_000_000)
    machine.periph.RCC.RCC_BDCR.LSION = 1
    machine.periph.RCC.RCC_BDCR.LSCOEN = 1
    await machine.run(for_=ms(1))

    assert chip.board.pin_clock("PA2") == 31_550
    assert [hz for hz, _ in chip.lpo] == [31_550]
    # --8<-- [end:pin-clock]


# --8<-- [start:codec]
class Codec(UartDevice):
    @override
    def on_attach(self) -> None:
        self.frames: list[AudioFrame] = []
        self.stamps: list[VirtualTime] = []
        self.heard = self.board.audio_port("SAI1", "A", self.on_audio)
        self.speaking: DeviceAudio = self.board.audio_port("SAI1", "B", self.on_audio)
        self.speaking.write([0x1000, 0x2000])

    def on_audio(self, frame: AudioFrame) -> None:
        self.frames.append(frame)
        self.stamps.append(self.now)


# --8<-- [end:codec]


class QuietCodec(Codec):
    @override
    def snapshot(self) -> bytes:
        return b""

    @override
    def restore(self, data: bytes) -> None:
        pass


async def sai_machine(codec: UartDevice) -> popcorn.Machine:
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(SAI_ELF),
        devices={"USART1": [codec]},
    )
    machine.reset()
    await machine.run(until="main", budget=100_000_000)
    return machine


async def test_a_heard_audio_frame_and_a_later_write_are_traffic() -> None:
    codec = Codec()
    machine = await sai_machine(codec)
    machine.snapshot()
    codec.speaking.write([1, 2])
    with pytest.raises(SnapshotRefused):
        machine.snapshot()

    heard = Codec()
    machine = await sai_machine(heard)
    machine.reg.pc = machine.sym("duplex_irq")
    await machine.run(budget=100_000_000)
    assert heard.frames
    with pytest.raises(SnapshotRefused):
        machine.snapshot()


async def test_a_restore_drops_queued_words_and_resets_starved() -> None:
    codec = QuietCodec()
    machine = await sai_machine(codec)
    snapshot = machine.snapshot()
    machine.reg.pc = machine.sym("duplex_irq")
    await machine.run(budget=100_000_000)
    assert codec.speaking.starved
    codec.speaking.write([1, 2])
    assert codec.speaking.queued == 2

    machine.restore(snapshot)
    assert (codec.speaking.queued, codec.speaking.starved) == (0, 0)


async def test_a_device_is_the_far_end_of_an_sai_block() -> None:
    # --8<-- [start:audio]
    codec = Codec()
    machine = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(SAI_ELF),
        devices={"USART1": [codec]},
    )
    machine.reset()
    await machine.run(until="main", budget=100_000_000)
    machine.reg.pc = machine.sym("duplex_irq")
    stop = await machine.run(budget=100_000_000)

    assert stop.reason is StopReason.USER
    assert codec.frames
    assert codec.stamps[0] - codec.frames[0].at == us(64)
    assert codec.speaking.queued == 0
    # --8<-- [end:audio]
