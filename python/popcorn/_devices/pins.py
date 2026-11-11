from __future__ import annotations

import typing
from collections.abc import Callable
from typing import cast, override

from popcorn import _core
from popcorn._devices.base import ATTACHED, BusDevice, serving
from popcorn._devices.naming import pin_of
from popcorn._devices.watches import Unwatch
from popcorn._enums import PinMode
from popcorn._errors import ConfigError, ModelError
from popcorn._limits import MAX_PS
from popcorn._virtual_loop import VirtualLoop
from popcorn.units import VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._audio import AudioFrame, Block, DeviceAudio


class _PinMachine(typing.Protocol):
    loop: VirtualLoop
    time: VirtualTime

    def _device_pin_watch(  # pyright: ignore[reportPrivateUsage]
        self, port: str, pin: int, handler: Callable[[bool, int], None]
    ) -> Unwatch: ...

    def _device_pin_clock_watch(  # pyright: ignore[reportPrivateUsage]
        self, port: str, pin: int, handler: Callable[[int, int], None]
    ) -> Unwatch: ...

    def _device_pin_clock(  # pyright: ignore[reportPrivateUsage]
        self, port: str, pin: int
    ) -> int: ...

    def _device_audio(  # pyright: ignore[reportPrivateUsage]
        self,
        owner: BusDevice,
        sai: str,
        block: str,
        handler: Callable[[int], None],
        claimed: Callable[[DeviceAudio], None],
    ) -> DeviceAudio: ...

    def _device_pin_read(  # pyright: ignore[reportPrivateUsage]
        self, port: str, pin: int
    ) -> tuple[bool, PinMode]: ...

    def _device_pin_drive(  # pyright: ignore[reportPrivateUsage]
        self, port: str, pin: int, level: bool, at_ps: int
    ) -> None: ...


class DeviceBoard:
    """What a bus device may drive on the board of the machine it serves, `target.board`.

    It holds no machine: each call finds the live machine through the device's weak
    attachment, so a device keeping this object keeps nothing alive.

    Pins are named by package pin, "PA5", and refused when the part's package does not bond them.
    An edge callback runs on the machine's thread in the machine's `VirtualLoop` at the edge's
    stamp, as the hooks of a `UartDevice` do. The stamp counts from the last reset of any kind, as
    `Machine.time`.
    """

    __slots__ = ("_device",)

    def __init__(self, device: BusDevice) -> None:
        """Act for `device`. Use `I2CTarget.board` or `SpiTarget.board` instead."""
        self._device = device

    def set_audio_clock(self, sai: str, block: Block, frame_hz: int) -> None:
        """Start, change or stop the external frame clock of an SAI block, as
        `AudioStream.external_frame_hz` does, from a bus callback at its exact virtual time.

        Raises:
            ConfigError: The device serves no live machine, or the board does not wire the
                block's clock to this device, `AudioOptions.clocked_by`.
            ValueError: `frame_hz` is outside 0 to 2**32 - 1.
        """
        held = ATTACHED.get(id(self._device))
        clock = None if held is None else held[1]()
        if clock is None:
            raise ConfigError(
                f"{self._device!r} serves no live machine",
                key="devices",
                expected="a device serving a live machine",
            )
        clock(self._device, sai, block, frame_hz)

    def audio_port(
        self, sai: str, block: Block, on_frame: Callable[[AudioFrame], None]
    ) -> DeviceAudio:
        """Own an SAI audio block as its far end, the guest being the master. `on_frame(frame)` is
        called for every frame a transmitting block sends, and the returned `DeviceAudio` queues
        the words a receiving block takes. See `DeviceAudio`. A device owning both blocks of an
        SAI calls this once for each. Calling again for a block the device owns replaces its
        callback, as a restored stack does.

        `on_frame` runs on the machine's thread in the machine's `VirtualLoop` at the frame's end
        stamp, whichever thread made the port. A frame heard from a `UartDevice` counts as
        traffic, and so does a `DeviceAudio.write` outside `on_attach`, so its default `snapshot`
        refuses: override `snapshot` and `restore`. A closed loop drops the frames.

        Raises:
            TypeError: `on_frame` is not callable.
            ConfigError: The device serves no live machine, `block` is neither "A" nor "B", or the
                block has an owner, an `AudioStream` of it or another device.
            ModelError: The part has no such built-in SAI.
        """
        if not callable(cast(object, on_frame)):
            raise TypeError(f"audio_port() takes a callable, not {on_frame!r}")
        from popcorn._audio import _audio_frames  # pyright: ignore[reportPrivateUsage]

        machine = self._machine()
        loop = machine.loop
        from popcorn._devices.uart import UartDevice

        stack = self._device if isinstance(self._device, UartDevice) else None
        taken: list[DeviceAudio] = []

        def heard(end_ps: int) -> None:
            frames = _audio_frames(taken[0]._link.drain(1))  # pyright: ignore[reportPrivateUsage]
            if not frames or loop.is_closed():
                return
            frame = frames[0]

            def call() -> None:
                if stack is not None:
                    stack._note_traffic()  # pyright: ignore[reportPrivateUsage]
                self._called("on_frame", on_frame, frame)

            loop._run_hook(end_ps, call)  # pyright: ignore[reportPrivateUsage]

        claim = machine._device_audio  # pyright: ignore[reportPrivateUsage]
        return claim(self._device, sai, block, heard, taken.append)

    def watch_pin(self, pin: str, on_edge: Callable[[bool, VirtualTime], None]) -> Unwatch:
        """Call `on_edge(level, at)` at every level change of `pin`, whoever causes it: a firmware
        store, a pin drive of the board, a device's `drive_pin`. `level` is the new level the input
        data register reads. `at` is the edge time EXTI and the timers see, in the loop step that
        runs the callback, where `device.loop` and `get_running_loop()` are the machine's loop.

        An edge from a firmware store is posted as a machine event at its stamp, so the callback
        runs at the first event boundary at or after it and never inside the store. A register
        access stamps at the end of its block, so `at` follows the block shape, as `UartDevice`'s
        `on_error`. An edge of a pin drive stamps at its exact time. At a tie the callbacks run in
        the order the edges were posted, after the model events due earlier.

        Reading a pin inside the callback gives its level at the callback's dispatch, after every
        store of the block that ended there. Two stores of one block share one stamp, so the
        callback of the first reads the level the second left. To know another pin's level at an
        edge, watch that pin too and keep its level from its own callback. The callbacks of one
        block come in store order, so the kept level is the one after the stores before this edge,
        also inside a block. A pin nobody watches costs no event and no GIL entry.
        The watch stays across every machine reset and restore, which change levels with no edge,
        so read `pin_level` after one. A reset drops the edges still pending. A restore brings back
        those its snapshot holds, and the watches standing hear them as the run that went on heard
        them. A snapshot holds no watch, so a new machine has none until a stack makes it. When a
        `UartDevice`'s default `restore` runs `on_attach` again, the watches the device made from
        the loop end just before it, after those edges, as `on_attach` makes them again.

        A `UartDevice`'s watch made from the loop belongs to its stack: an edge delivered to it
        after the attach stamp counts as traffic, so the default `snapshot` refuses, since a stack
        started again would lose what the edge told it. That holds for an edge pending at a restore
        too, which reaches the old watch ahead of the rerun, as the run that went on heard it, so
        both refuse the next default `snapshot`. What that edge arms stays beside the new stack, so
        a stack that arms from the edge and from the level in `on_attach` checks it armed once. At
        the attach stamp an edge does not count: a rerun reads the same level with `pin_level`. A
        watch made from the test thread counts nothing. A device that overrides `snapshot` decides
        for itself.

        `on_edge` runs in the context of the machine's hooks, whichever thread made the watch, see
        `Machine.restore`.

        Raises:
            TypeError: `pin` is no str or `on_edge` not callable.
            ValueError: `pin` is no pin name such as "PA5", pin 0 to 15.
            ConfigError: The device serves no live machine, or the port has no such pin or the
                package does not bond it.
            ModelError: The part has no such GPIO port.
            MachineRunning: The machine is busy, outside its callbacks.
        """
        port, number = pin_of(pin)
        if not callable(cast(object, on_edge)):
            raise TypeError(f"watch_pin() takes a callable, not {on_edge!r}")
        machine = self._machine()
        loop = machine.loop
        stamp_of = loop._stamp_since_reset  # pyright: ignore[reportPrivateUsage]
        stepping = loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
        from popcorn._devices.uart import UartDevice

        stack = self._device if stepping and isinstance(self._device, UartDevice) else None

        def edge(level: bool, at_ps: int) -> None:
            def held_at_the_stamp() -> None:
                if stack is not None:
                    stack._note_pin()  # pyright: ignore[reportPrivateUsage]
                self._called("on_edge", on_edge, level, VirtualTime(stamp_of()))

            loop._run_hook(at_ps, held_at_the_stamp)  # pyright: ignore[reportPrivateUsage]

        watch = machine._device_pin_watch(port, number, edge)  # pyright: ignore[reportPrivateUsage]
        if stack is not None:
            stack._note_watch(watch)  # pyright: ignore[reportPrivateUsage]
        return watch

    def drive_pin(self, pin: str, level: bool, *, at: VirtualTime | None = None) -> None:
        """Drive `pin` to `level` from outside the MCU at `at`, or at `now` when None, as
        `Pin.drive_at` does: the level reaches the input data register at the next event dispatch,
        the pin watchers, EXTI and the timers see the edge at `at`, and it is board state kept
        across a machine reset. In a hook or a loop callback `now` is the held stamp,
        `UartDevice.now`. It works from the hooks of the device, `reset` included, where `now` is
        the reset instant, and from the callbacks of the machine's `VirtualLoop`, as
        `UartDevice.send` does, so a call from the test while the machine is parked goes through
        `Machine.loop.call_soon`, which makes it a timer at the machine's clock.

        From a `UartDevice` after the attach stamp it counts as traffic, so the default `snapshot`
        refuses, since a rerun of `on_attach` would drive the firmware's input again. So does a
        call at the attach stamp with `at` past it. A drive at the attach stamp does not count: a
        rerun repeats it at the restored clock. A drive from `restore` does not count either, at
        any place against `super().restore`, unless its `at` lies past the restored clock: every
        restore repeats it.

        Raises:
            TypeError: `level` is no bool, or `at` no `VirtualTime`.
            ValueError: `pin` is no pin name, or `at` lies before now or leaves the 64-bit
                picosecond range.
            RuntimeError: Called off the machine's thread outside the device's hooks and the
                loop's callbacks.
            ConfigError: As `watch_pin`, or the port keeps as many pending edges as it can.
            ModelError: As `watch_pin`.
        """
        port, number = pin_of(pin)
        if not isinstance(cast(object, level), bool):
            raise TypeError(f"drive_pin() takes level as a bool, not {level!r}")
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"drive_pin() takes at as a VirtualTime or None, not {at!r}")
        machine = self._machine()
        loop = machine.loop
        stepping = loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
        if not stepping and not _core.on_host_thread():
            raise RuntimeError(f"{self!r}: drive_pin() runs on the machine's thread")
        now = (
            loop._stamp_since_reset()  # pyright: ignore[reportPrivateUsage]
            if stepping
            else machine.time.ps
        )
        start = now if at is None else at.ps
        if start < now:
            raise ValueError(f"{self!r}: drive_pin() at {at} lies before now, {VirtualTime(now)}")
        if start > MAX_PS:
            raise ValueError(f"a drive_pin at {at} leaves the 64-bit picosecond range")
        machine._device_pin_drive(port, number, level, start)  # pyright: ignore[reportPrivateUsage]
        device = self._device
        from popcorn._devices.uart import UartDevice

        if isinstance(device, UartDevice):
            device._note_pin(later=start > now)  # pyright: ignore[reportPrivateUsage]

    def pin_level(self, pin: str) -> bool:
        """Level of `pin` as its input data register reads it, whatever its mode. An alternate
        function pin reads its GPIO level, not the peripheral's signal, so read `pin_mode` and
        `UartDevice.rts` for RTS.

        Raises:
            TypeError: `pin` is no str.
            ValueError: `pin` is no pin name.
            ConfigError: As `watch_pin`.
            ModelError: As `watch_pin`.
            MachineRunning: The machine is busy, outside its callbacks.
        """
        port, number = pin_of(pin)
        return self._pin_state(port, number)[0]

    def pin_clock(self, pin: str) -> int:
        """Frequency in Hz the MCU drives onto `pin` as a clock output, 0 when none: the low-speed
        clock output LSCO of `RCC_BDCR` on PA2 of the STM32U5 and the STM32L4 and on PB2 of the
        STM32H5, LSI or LSE as `LSCOSEL` picks it, when `LSCOEN` is set. The STM32U5 LSE needs
        `LSESYSEN` and `LSESYSRDY`. The LSI rate is the family nominal or `RccOptions.lsi_hz`.
        The answer does not depend on `pin_mode` or `pin_level`: the pin carries the clock as an
        additional function. That the pin needs no GPIO mode is inferred, RM0351 6.2.17 and
        RM0481 11.4.11 only say the LSCO pin is left in its reset state.

        Raises:
            TypeError: `pin` is no str.
            ValueError: `pin` is no pin name.
            ConfigError: As `watch_pin`.
            ModelError: As `watch_pin`.
            MachineRunning: The machine is busy, outside its callbacks.
        """
        port, number = pin_of(pin)
        read = self._machine()._device_pin_clock  # pyright: ignore[reportPrivateUsage]
        return read(port, number)

    def watch_pin_clock(self, pin: str, on_change: Callable[[int, VirtualTime], None]) -> Unwatch:
        """Call `on_change(hz, at)` at every change of `pin_clock(pin)`, with the new rate. `at` is
        the time of the model event that carries the change, the first instruction boundary at or
        after it, as the `at` of `watch_pin`: a register write stamps at the end of its block, a
        source that was still starting stamps at the first boundary at or after its ready instant,
        100 us after the write for the LSI. With no instruction between, a parked write stamps at
        the write time. The call runs in the loop step that follows, never inside the write, and a
        write that leaves the rate as it was calls nothing. A pin that carries no clock output
        never calls. A watch stays across resets and restores. A reset drops the calls still
        pending, then calls once with the new rate, at the reset time, when it changed the rate. A
        restore changes the rate with no call, and brings back the calls its snapshot holds
        pending, which the standing watches hear, so read `pin_clock` after one. Delivery,
        threading and the `UartDevice` stack rules are those of `watch_pin`.

        Raises:
            TypeError: `pin` is no str or `on_change` not callable.
            ValueError: `pin` is no pin name such as "PA5", pin 0 to 15.
            ConfigError: As `watch_pin`.
            ModelError: As `watch_pin`.
            MachineRunning: The machine is busy, outside its callbacks.
        """
        port, number = pin_of(pin)
        if not callable(cast(object, on_change)):
            raise TypeError(f"watch_pin_clock() takes a callable, not {on_change!r}")
        machine = self._machine()
        loop = machine.loop
        stamp_of = loop._stamp_since_reset  # pyright: ignore[reportPrivateUsage]
        stepping = loop._stepping_here()  # pyright: ignore[reportPrivateUsage]
        from popcorn._devices.uart import UartDevice

        stack = self._device if stepping and isinstance(self._device, UartDevice) else None

        def change(hz: int, at_ps: int) -> None:
            def held_at_the_stamp() -> None:
                if stack is not None:
                    stack._note_pin()  # pyright: ignore[reportPrivateUsage]
                self._called("on_change", on_change, hz, VirtualTime(stamp_of()))

            loop._run_hook(at_ps, held_at_the_stamp)  # pyright: ignore[reportPrivateUsage]

        watch = machine._device_pin_clock_watch(  # pyright: ignore[reportPrivateUsage]
            port, number, change
        )
        if stack is not None:
            stack._note_watch(watch)  # pyright: ignore[reportPrivateUsage]
        return watch

    def pin_mode(self, pin: str) -> PinMode:
        """Mode the firmware configured for `pin`. An input is released, a push-pull output driven,
        an alternate function belongs to a peripheral, and an analog pin reads 0. An open-drain
        output reads `OUTPUT` whatever its output data bit: at 1 the pin is released and
        `pin_level` follows the pulls, 0 with none, so `OUTPUT` at level 0 can be driven low or
        released with no pull. What separates an output driven low from a pin the firmware
        switched to input is the mode.

        Raises:
            TypeError: `pin` is no str.
            ValueError: `pin` is no pin name.
            ConfigError: As `watch_pin`.
            ModelError: As `watch_pin`.
            MachineRunning: The machine is busy, outside its callbacks.
        """
        port, number = pin_of(pin)
        return self._pin_state(port, number)[1]

    def _pin_state(self, port: str, number: int) -> tuple[bool, PinMode]:
        read = self._machine()._device_pin_read  # pyright: ignore[reportPrivateUsage]
        return read(port, number)

    def _machine(self) -> _PinMachine:
        machine = serving(self._device)
        if machine is None:
            raise ConfigError(
                f"{self._device!r} serves no live machine",
                key="devices",
                expected="a device serving a live machine",
            )
        return cast(_PinMachine, machine)

    def _called[**P](
        self, name: str, hook: Callable[P, None], *args: P.args, **kwargs: P.kwargs
    ) -> None:
        try:
            hook(*args, **kwargs)
        except Exception as error:
            held = ATTACHED.get(id(self._device))
            raise ModelError(
                f"{self._device!r}: {name} raised {error!r}",
                peripheral="" if held is None else held[5],
            ) from error

    @override
    def __repr__(self) -> str:
        return f"DeviceBoard({self._device!r})"
