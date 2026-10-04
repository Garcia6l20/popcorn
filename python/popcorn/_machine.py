"""The emulated machine and its asyncio slice driver."""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import difflib
import functools
import os
import threading
import types
from collections.abc import AsyncGenerator, Callable, Generator, Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Protocol, cast, overload, runtime_checkable

from popcorn import _core
from popcorn._arena import Arena
from popcorn._audio import AudioOptions, AudioPorts, Block
from popcorn._call import (
    F32,
    I32_SLOT,
    I64,
    CallResult,
    Signature,
    Slot,
    Stack,
    argument,
    decode,
    encode,
    signature_of,
    slot_for,
)
from popcorn._description import MachineDescription, load_description
from popcorn._devices import (
    BusDevice,
    I2CTarget,
    SpiTarget,
    attach,
    cs_of,
    serving,
)
from popcorn._dwarf_views import Var
from popcorn._enums import FloatAbi, ResetKind, StopReason, ValueKind
from popcorn._errors import (
    BudgetExceeded,
    ConfigError,
    EmulationError,
    FaultError,
    MachineBusy,
    MachineRunning,
    SymbolError,
)
from popcorn._hooks import (
    CallbackTable,
    Initiator,
    Intercept,
    LogEntry,
    Relay,
    Return,
    Returned,
    Watch,
    WatchAction,
    WatchHit,
    WatchKind,
)
from popcorn._image import Image, Location, Symbol, location_of
from popcorn._model_build import build_model
from popcorn._nvic import Irqs, Nvic
from popcorn._observe import MachineCoverage, Ranges, Ring, TraceRecording, forward_log, open_trace
from popcorn._rcc import RccOptions
from popcorn._snapshot import Snapshot
from popcorn._stop import Stop, stop_of
from popcorn._streams import Board, UartPorts
from popcorn._usb import UsbHostOptions, UsbHosts, checked_usb
from popcorn._views import Memory, Peripherals, ViewAccess
from popcorn.arm import Registers
from popcorn.devices import At25sf, Eeprom24xx, TableTarget
from popcorn.model import Model
from popcorn.trace import TraceKind
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn.image.dwarf import DebugInfo, DebugType


class Call:
    """A call arriving at an intercepted function, stopped at its first instruction.

    Valid only while the handler runs.
    """

    __slots__ = ("_signature", "_view", "machine")

    def __init__(self, machine: Machine, view: _core.InterceptCall, signature: Signature) -> None:
        """Wrap a native call. Handlers receive it built."""
        self.machine = machine
        self._view = view
        self._signature = signature

    @property
    def function(self) -> int:
        """Address of the intercepted function."""
        return self._view.function()

    @property
    def occurrence(self) -> int:
        """Calls of this intercept before this one, 0 for the first."""
        return self._view.occurrence()

    @property
    def lr(self) -> int:
        """Address the function returns to."""
        return self._view.return_address()

    @property
    def sp(self) -> int:
        """Stack pointer at entry."""
        return self.machine.reg.sp

    def args(
        self, kinds: Sequence[ValueKind], abi: FloatAbi | None = None
    ) -> tuple[int | float, ...]:
        """Arguments of the given kinds, integers signed, floats decoded, under the image's
        calling convention or `abi`."""
        native = [_core.ValueKind(k.value) for k in kinds]
        bits = self._view.args(native, None if abi is None else _core.FloatAbi(abi.value))
        return tuple(decode(slot_for(k), b) for k, b in zip(kinds, bits, strict=True))

    def args_typed(self, abi: FloatAbi | None = None) -> tuple[int | float, ...]:
        """Arguments read with the function's DWARF signature, each at its C type.

        Raises:
            TypeError: The function has no DWARF signature.
        """
        params = self._signature.params
        if params is None:
            reason = self._signature.unsupported or f"{self._signature.name} has no DWARF signature"
            raise TypeError(f"{reason}, use args(kinds)")
        native = [p.kind for p in params]
        bits = self._view.args(native, None if abi is None else _core.FloatAbi(abi.value))
        return tuple(decode(p, b) for p, b in zip(params, bits, strict=True))

    def arg(self, index: int) -> int:
        """Argument `index` as an integer: from DWARF when known, else the index-th 32-bit
        integer argument."""
        if self._signature.params is not None:
            value = self.args_typed()[index]
            if not isinstance(value, int):
                raise TypeError(f"argument {index} of {self._signature.name} is a float")
            return value
        value = self.args([ValueKind.I32] * (index + 1))[index]
        assert isinstance(value, int)
        return value

    def arg_f(self, index: int) -> float:
        """Argument `index` as a float, from the DWARF signature."""
        value = self.args_typed()[index]
        if not isinstance(value, float):
            raise TypeError(f"argument {index} of {self._signature.name} is an integer")
        return value

    def proceed(self) -> Return:
        """Run the function as if no intercept were there."""
        return Return(_core.InterceptResult.proceed())

    def returns(
        self, value: int | float | I64 | F32 | None = None, abi: FloatAbi | None = None
    ) -> Return:
        """Skip the function and return `value` to the caller, of the DWARF return type when
        known, under the image's calling convention or `abi`. None for a void function."""
        if value is None:
            return Return(_core.InterceptResult.returns(None))
        slot = self._result_slot(value)
        number = value.value if isinstance(value, I64 | F32) else value
        arg = _core.CallArg()
        arg.kind = slot.kind
        arg.bits = encode(slot.kind, number, f"the return value of {self._signature.name}")
        native_abi = None if abi is None else _core.FloatAbi(abi.value)
        return Return(_core.InterceptResult.returns(arg, native_abi), number)

    def stop(self, reason: StopReason = StopReason.USER) -> Return:
        """Stop the run at the function, before its body.

        Raises:
            ValueError: `reason` is FAULT, which only the core raises.
        """
        if reason is StopReason.FAULT:
            raise ValueError("an intercept cannot stop with FAULT")
        return Return(_core.InterceptResult.stop(_core.StopReason(reason.value)))

    def _result_slot(self, value: int | float | I64 | F32) -> Slot:
        if isinstance(value, I64):
            return slot_for(ValueKind.I64)
        if isinstance(value, F32):
            return slot_for(ValueKind.F32)
        if self._signature.params is not None:
            return self._signature.returns
        return slot_for(ValueKind.F64 if isinstance(value, float) else ValueKind.I32)


type InterceptHandler = Callable[[Call], Return]
"""An intercept handler: the call in, what to do out."""
type ReturnHandler = Callable[[Returned], None]
"""An on_return handler."""
type WatchHandler = Callable[[WatchHit], WatchAction | None]
"""A watch handler: STOP, RESUME, or None for RESUME."""

DEFAULT_ARENA_BASE = 0x6F00_0000
"""Default first address of the call arena, an FMC address STM32 parts leave unmapped."""
DEFAULT_ARENA_SIZE = 1 << 20
"""Default size of the call arena."""

_HANDLER_OF: contextvars.ContextVar[Machine | None] = contextvars.ContextVar(
    "popcorn_handler_of", default=None
)


def _check_duration(for_: VirtualDuration | None) -> None:
    if for_ is not None and for_.ps < 0:
        raise ValueError(f"for_ must not be negative, got {for_}")


def _stop_error(stopped: Stop, during: str = "the background run") -> EmulationError:
    message = f"{during} stopped with {stopped.reason.name}: {stopped}"
    if stopped.reason is StopReason.FAULT and stopped.fault is not None:
        fault = stopped.fault
        return FaultError(
            message,
            kind=fault.kind,
            address=fault.address,
            cfsr=fault.cfsr,
            hfsr=fault.hfsr,
            pc=stopped.pc,
            symbol=stopped.location.symbol,
            stop=stopped,
        )
    if stopped.reason is StopReason.BUDGET:
        return BudgetExceeded(
            message,
            budget=stopped.instructions,
            pc=stopped.pc,
            symbol=stopped.location.symbol,
            stop=stopped,
        )
    return EmulationError(
        message,
        reason=stopped.reason,
        pc=stopped.pc,
        symbol=stopped.location.symbol,
        stop=stopped,
    )


@runtime_checkable
class Event(Protocol):
    """Something `Machine.wait_for` waits for.

    `ready` is sync and runs at event boundaries and slice ends, never per instruction. It may
    read the machine, which is stopped at an instruction boundary while it runs.
    """

    def ready(self, machine: Machine) -> bool:
        """Whether the event happened."""
        ...


@dataclass(frozen=True, slots=True)
class Reached:
    """The core reaching an address or symbol, before executing it.

    `wait_for` stops exactly there, outside `running()`. Inside `running()` it is checked at slice
    boundaries like any other event.
    """

    target: int | str | Symbol

    def ready(self, machine: Machine) -> bool:
        """Whether pc is at the target."""
        return machine.pc == machine.address_of(self.target)


@dataclass(frozen=True, slots=True)
class _Ready:
    predicate: Callable[[], bool]

    def ready(self, machine: Machine) -> bool:
        return self.predicate()


@dataclass(slots=True)
class _Waiter:
    event: Event
    deadline_ps: int | None
    future: asyncio.Future[Stop]


class Machine:
    """One emulated MCU: a description, the images loaded into it and its run state.

    Built powered off: nothing runs until `reset()`. Emulation advances in slices on the event
    loop thread, each bounded by `slice_instructions` and `slice_wall_budget`, yielding to the
    loop between slices. Sync access to the machine while a run is active raises
    MachineRunning, except from a handler of that run.
    """

    __slots__ = (
        "__weakref__",
        "_arena",
        "_audio",
        "_background",
        "_board",
        "_busy",
        "_call_baseline",
        "_callback_table",
        "_config_digest",
        "_coverage",
        "_default_budget",
        "_description",
        "_devices",
        "_handle",
        "_images",
        "_installed",
        "_irqs",
        "_last_stop",
        "_log_hook",
        "_logs",
        "_memory",
        "_nvic",
        "_peripherals",
        "_ports",
        "_registers",
        "_ring",
        "_sentinel",
        "_slice_instructions",
        "_slice_wall_ns",
        "_thread",
        "_usb",
        "_waiters",
    )

    def __init__(
        self,
        description: MachineDescription | str | os.PathLike[str],
        *,
        image: Image | Iterable[Image] = (),
        core_hz: int = 0,
        cpi: float = 1.0,
        trace_capacity: int = 4096,
        spin_threshold: int = 0,
        strict_peripherals: bool = False,
        vector_table: int | None = None,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
        config_digest: str = "",
        arena_base: int | None = DEFAULT_ARENA_BASE,
        arena_size: int = DEFAULT_ARENA_SIZE,
        models: Mapping[str, Callable[[], Model]] | None = None,
        devices: (
            Mapping[str, Sequence[I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget]] | None
        ) = None,
        rcc: RccOptions | None = None,
        audio: Mapping[str, Mapping[Block, AudioOptions]] | None = None,
        usb: Mapping[str, UsbHostOptions] | None = None,
    ) -> None:
        """Build a machine from a description or the path of its JSON, then load `image`.

        Args:
            description: The description, or the path of its JSON file.
            image: Image or images to load, in order.
            core_hz: Core clock at power-on in Hz, 0 for the description's reset clock.
            cpi: Core cycles per instruction, above 0.
            trace_capacity: Records the always-on trace ring keeps, above 0.
            spin_threshold: Iterations of a loop making no progress before a SPIN stop, 0 off.
            strict_peripherals: Whether an unmodelled register access raises ModelError.
            vector_table: Address of the vector table reset reads, None for the default.
            slice_instructions: Instructions after which a slice yields to the event loop.
            slice_wall_budget: Wall seconds after which a slice yields to the event loop.
            config_digest: Digest of the configuration, such as a project file's, that snapshots
                record.
            arena_base: First address of the call arena, a RAM region outside the description,
                None for no arena. It must not overlap a region or a peripheral.
            arena_size: Size of the call arena, a multiple of 4 KiB.
            models: Python model per peripheral name, a `popcorn.model.Model` subclass or a
                callable building one, such as `functools.partial(Timer, period=5)`. It
                replaces the registered model of that peripheral.
            devices: Bus devices per I2C or SPI peripheral name, `popcorn.board` targets or
                `popcorn.devices` devices of one bus kind, in attachment order. The built-in bus
                model of the peripheral serves them. A device serves one live machine at a time.
            rcc: Clock sources the board gives the built-in RCC model, None for its defaults,
                a 16 MHz HSE and a 32.768 kHz LSE.
            audio: Options of SAI audio blocks by SAI instance and block, such as
                `{"SAI1": {"A": AudioOptions(decimation=4)}}`. An alias, such as `SEC_SAI1`,
                names the SAI serving it. Blocks not listed get `AudioOptions()`.
            usb: Virtual USB host per USB device controller, such as
                `{"OTG_FS": UsbHostOptions()}`, built with the machine and owned by the built-in
                model of the controller. An alias names the controller serving it. A controller
                not listed has no host: it sees no VBUS and never enumerates.

        Raises:
            ConfigError: No arch module implements the core, an option is out of range, a
                models key names no peripheral, a model is refused, or devices are refused:
                a peripheral that is not a built-in bus of their kind or has a Python model,
                a chip select pin the description lacks, a device already serving a live
                machine or listed twice. With `rcc`: an HSE outside 4 to 50 MHz, an LSE of 0 Hz
                or above 1 MHz, an RCC with a Python model or one no built-in model serves.
                With `audio`: a key that is not an SAI with the built-in model, two keys naming
                one SAI, a block other than A or B, a decimation below 1 or a negative frame
                rate, named as `audio.<SAI>.<block>`. With `usb`: a key that is not a USB device
                controller with the built-in model, two keys naming one controller, a bus reset
                or a timeout not above 0, named as `usb.<NAME>`.
            ImageError: An image does not fit the memory map.
        """
        if slice_instructions <= 0 or not slice_wall_budget > 0:
            raise ValueError("slice_instructions and slice_wall_budget must be above 0")
        if not isinstance(description, MachineDescription):
            description = load_description(description)
        config = _core.MachineConfig()
        config.core_hz = core_hz
        config.cpi = cpi
        config.trace_capacity = trace_capacity
        config.spin_threshold = spin_threshold
        config.strict_peripherals = strict_peripherals
        config.vector_table = vector_table
        config.config_digest = config_digest
        config.arena_start = arena_base
        config.arena_size = arena_size if arena_base is not None else 0
        config.python_models = _model_makers(description, models or {})
        attached = _checked_devices(description, devices or {}, models or {})
        config.python_devices = {name: list(targets) for name, targets in attached.items()}
        if rcc is not None:
            config.rcc_hse_hz = rcc.hse_hz
            config.rcc_lse_hz = rcc.lse_hz
        aliases = {p.name: p.alias_of for p in description.peripherals if p.alias_of is not None}
        config.usb_hosts = checked_usb(usb or {}, aliases)
        self._description = description
        self._config_digest = config_digest
        self._handle = _core.MachineHandle(description, config)
        self._images: list[Image] = []
        self._slice_instructions = slice_instructions
        self._slice_wall_ns = max(1, round(slice_wall_budget * 1e9))
        self._busy = False
        self._background = False
        self._waiters: list[_Waiter] = []
        self._thread = threading.get_ident()
        self._ports = UartPorts(
            self._handle.attach_uart,
            self._handle.uart_names(),
            {p.name: p.alias_of for p in description.peripherals if p.alias_of is not None},
            self._wait_until,
            self._require_owner,
        )
        sais = self._handle.audio_names()
        self._audio = AudioPorts(
            self._handle.attach_audio,
            sais,
            aliases,
            _checked_audio(audio or {}, sais, aliases),
            self._wait_until,
            self._require_owner,
        )
        self._usb = UsbHosts(
            self._handle.attach_usb,
            self._handle.usb_names(),
            aliases,
            self._wait_until,
            self._require_owner,
            self._ports.bridges,
        )
        self._devices = attached
        for targets in attached.values():
            for target in targets:
                attach(target, self)
        self._board = Board(self._ports, attached, self._usb)
        access = ViewAccess(
            self._handle,
            self._require_stopped,
            self._ports.stream,
            self._require_owner,
            self._audio.stream,
            self._usb.host,
        )
        self._memory = Memory(access)
        self._registers = Registers(access, self._handle.register_names())
        self._peripherals = Peripherals(access, description)
        self._nvic = Nvic(self._memory, description.core.num_irqs)
        self._coverage = MachineCoverage(self._handle, lambda: self._images, self._require_stopped)
        self._ring = Ring(self._handle, self.locate, self._require_stopped, self._require_idle)
        self._log_hook = self._handle.add_log_sink(functools.partial(forward_log, description.part))
        self._irqs = Irqs(description)
        self._arena = (
            None
            if arena_base is None
            else Arena(self._memory, arena_base, arena_size, self._type_view)
        )
        self._call_baseline: Snapshot | None = None
        self._last_stop: Stop | None = None
        self._default_budget: int | None = None
        self._logs: dict[int, list[LogEntry]] = {}
        self._callback_table = CallbackTable()
        self._installed: list[Intercept | Watch] = []
        self._sentinel = (
            self._arena.sentinel if self._arena is not None else _free_sentinel(description)
        )
        for one in (image,) if isinstance(image, Image) else image:
            self.load(one)

    @property
    def description(self) -> MachineDescription:
        """Description the machine was built from."""
        return self._description

    @property
    def config_digest(self) -> str:
        """Digest of the configuration the machine was built from, empty when none."""
        return self._config_digest

    @property
    def images(self) -> tuple[Image, ...]:
        """Images loaded so far, in load order."""
        return tuple(self._images)

    @property
    def busy(self) -> bool:
        """Whether a run, step, wait or `running()` is active."""
        return self._busy

    @property
    def instructions(self) -> int:
        """Instructions executed since reset."""
        self._require_stopped()
        return self._handle.instructions()

    @property
    def time(self) -> VirtualTime:
        """Virtual time since reset."""
        self._require_stopped()
        return VirtualTime(self._handle.time_ps())

    @property
    def pc(self) -> int:
        """Address of the next instruction."""
        self._require_stopped()
        return self._handle.pc()

    @property
    def pending_events(self) -> int:
        """Events waiting in the event queue, model and core events alike.

        A model event superseded without being cancelled counts until it is dispatched, so a
        count that grows with guest work shows a model leaving stale events behind.
        """
        self._require_stopped()
        return self._handle.pending_events()

    def load(self, image: Image) -> None:
        """Load one more image over memory, such as an application after a bootloader.

        Raises:
            ImageError: A segment falls outside the description's memory regions.
            MachineRunning: A run is active.
        """
        self._require_stopped()
        self._handle.load(image.native)
        self._images.append(image)

    def model(self, peripheral: str) -> Model | None:
        """The Python model serving a peripheral, None when it has none or a native one.

        Raises:
            ConfigError: The description has no such peripheral.
        """
        found: object = self._handle.model(peripheral)
        return found if isinstance(found, Model) else None

    def reset(self, kind: ResetKind = ResetKind.POWER_ON) -> None:
        """Reset: clock to 0, core from its vector table, peripherals to reset values.

        Memory is kept.

        Raises:
            MachineRunning: A run is active, handlers of it included.
        """
        self._require_stopped()
        self._handle.reset(_core.ResetKind(kind.value))
        self._last_stop = None

    @property
    def mem(self) -> Memory:
        """Guest memory, typed accessors included: `m.mem.u32[addr] = 5`."""
        return self._memory

    @property
    def reg(self) -> Registers:
        """Core registers as typed attributes, `m.reg.r0`, or by `popcorn.arm.Reg`."""
        return self._registers

    @property
    def coverage(self) -> MachineCoverage:
        """Block and edge coverage, `m.coverage.enable()` then `m.coverage.save(path)`."""
        return self._coverage

    @property
    def last_stop(self) -> Stop | None:
        """The stop that ended the most recent run, step, wait, call or `running()` background
        run. None after construction, `reset()`, `restore()`, and a run that ended without a
        stop: cancelled, or ended by an exception of a handler or predicate.

        Raises:
            MachineRunning: A run is active.
        """
        self._require_stopped()
        return self._last_stop

    @property
    def default_budget(self) -> int | None:
        """The instruction budget of `run`, `wait_for` and `call` when they are given none.

        None, the default, leaves them unbounded. `step` counts its own instructions, and the
        background run of `running()` stays unbounded whatever this is. A host setting, not part
        of snapshots.
        """
        return self._default_budget

    @default_budget.setter
    def default_budget(self, budget: int | None) -> None:
        self._default_budget = _checked_budget(budget)

    def remove_hooks(self) -> int:
        """Remove every intercept, return hook and watch of this machine, as `remove()` on each.
        Also empties every intercept log.

        Returns:
            How many were still installed.

        Raises:
            MachineRunning: A run is active, handlers of it included.
        """
        self._require_idle()
        live = [hook for hook in self._installed if hook.active]
        for hook in live:
            hook.remove()
        self._installed.clear()
        self._logs.clear()
        return len(live)

    @property
    def ring(self) -> Ring:
        """The always-on trace ring, `m.ring.dump()` after a failure."""
        return self._ring

    @contextlib.contextmanager
    def trace(
        self,
        kinds: Iterable[TraceKind],
        path: str | os.PathLike[str],
        *,
        ranges: Ranges | None = None,
        functions: Iterable[str] | None = None,
    ) -> Generator[TraceRecording]:
        """Write a full `.pctr` trace of `kinds` while the block runs, `popcorn.trace.ALL` for
        every kind.

        `ranges` and `functions` limit the trace to records caused by that code, everywhere
        when both are None. Block and memory records cost a native hook while traced, nothing
        crosses to Python per record. Leaving the block removes the sink, then closes the file.

        Raises:
            ConfigError: A kind is not selectable, or a range is invalid.
            SymbolError: No loaded image has a function name.
            FormatError: The file cannot be created, written or closed, from `popcorn._obs`.
            MachineRunning: A run is active.
        """
        self._require_stopped()
        recording = open_trace(self._handle, kinds, path, ranges, functions)
        try:
            yield recording
        except BaseException as error:
            try:
                recording.close()
            except Exception as close_error:
                error.add_note(f"closing the trace {recording.path} also failed: {close_error}")
            raise
        recording.close()

    @property
    def board(self) -> Board:
        """UART streams and host ptys wired to the MCU."""
        return self._board

    @property
    def nvic(self) -> Nvic:
        """The interrupt controller from the debugger's side."""
        return self._nvic

    @property
    def irq(self) -> Irqs:
        """Interrupt numbers of the description by name, `m.irq.USART2_IRQn`."""
        return self._irqs

    @property
    def periph(self) -> Peripherals:
        """Peripheral registers by name, `m.periph.USART1.CR1.UE`.

        Reads and writes register storage without model effects; use m.mem or
        RegisterView.read/write for bus accesses a model reacts to.
        """
        return self._peripherals

    def sym(self, name: str) -> int:
        """Address of a symbol of the loaded images, the last loaded first.

        Raises:
            SymbolError: No loaded image has the name, with close matches.
        """
        return self.address_of(name)

    @property
    def symbols(self) -> dict[str, Symbol]:
        """Every symbol of the loaded images by name, a later image winning a clash."""
        merged: dict[str, Symbol] = {}
        for image in self._images:
            merged.update((s.name, s) for s in image.symbols)
        return merged

    def var(self, name: str) -> Var:
        """A module-scope variable of the loaded ELF images, typed from their DWARF.

        Raises:
            SymbolError: No loaded image declares the variable, with close matches.
        """
        known: list[str] = []
        for info in reversed(self._debug_infos()):
            variable = info.variables.get(name)
            if variable is not None:
                return Var(self._memory, info, variable.type, variable.address, name)
            known.extend(info.variables)
        close = difflib.get_close_matches(name, known, n=5)
        raise SymbolError(
            f"no variable {name} in the loaded images, close matches: {', '.join(close)}",
            name=name,
            close_matches=close,
        )

    @property
    def types(self) -> dict[str, DebugType]:
        """Named types of the loaded ELF images' DWARF, typedefs included, a later image winning
        a clash."""
        merged: dict[str, DebugType] = {}
        for info in self._debug_infos():
            merged.update((name, info.resolve(type_id)) for name, type_id in info.types.items())
        return merged

    def _debug_infos(self) -> list[DebugInfo]:
        from popcorn.image.dwarf import DebugInfoError, load_debug_info

        infos: list[DebugInfo] = []
        for image in self._images:
            if image.path is not None and image.path.suffix != ".hex" and image.segments:
                try:
                    infos.append(load_debug_info(image.path))
                except DebugInfoError:
                    continue
        return infos

    @property
    def arena(self) -> Arena:
        """The call arena, scratch memory for call arguments, reset by `restore`.

        Raises:
            ConfigError: The machine was built with no arena.
        """
        if self._arena is None:
            raise ConfigError(
                "no arena configured", file="machine", key="arena", expected="an arena_base"
            )
        return self._arena

    def set_call_baseline(self, snapshot: Snapshot | None) -> None:
        """The snapshot `call(reset=True)` restores, None to take one at the next such call."""
        self._call_baseline = snapshot

    async def call(
        self,
        fn: int | str | Symbol,
        *args: object,
        returns: ValueKind | None = None,
        budget: int | None = None,
        privileged: bool = True,
        stack: Stack = Stack.CURRENT,
        stack_top: int | None = None,
        interrupts: bool = False,
        reset: bool | Snapshot = False,
        abi: FloatAbi | None = None,
    ) -> CallResult:
        """Call a guest function and return what it returned, the interrupted context restored.

        The signature comes from DWARF when the image has the function: the argument count is
        checked before anything runs, and each argument takes its parameter's kind, range and
        signedness. Without DWARF an int is a 32-bit integer unless `popcorn.i64(v)`, a float a
        double unless `popcorn.f32(v)`, and `returns` names the result kind, I32 by default. An
        arena block or a variable passes its address. A struct by value is refused.

        When the call ends, the interrupted execution context comes back: the core registers,
        the special registers, the active exceptions, the sleep state and the interrupt masking.
        The System Control Space keeps the call's writes, as memory and the device peripherals
        do: NVIC enables and priorities, SCB configuration and fault status, SysTick, MPU, DWT
        and ITM. Interrupts made pending during the call stay pending. A synchronous exception
        the call left pending, SVC, HardFault, MemManage, BusFault or UsageFault, is dropped. A
        handler still active when the call ends is not resumed and its interrupt is not pended
        again. An exception that ends the call, a ModelError or one
        a handler raised, restores the same as a return, and the machine runs on from where it
        was.

        A FAULT that ends the call carries the faulting registers in `stop.registers`, taken
        before the restore, and `popcorn.report.stop_report` shows and unwinds those. The fault
        status registers stay set as on hardware: CFSR bits are sticky, so a later fault of the
        caller reports them OR'ed with its own until the firmware clears them.

        Args:
            fn: Function address, symbol or name.
            args: Arguments in declaration order.
            returns: Result kind of a function without DWARF.
            budget: Stop the call with BUDGET past this many instructions, `default_budget`
                when None.
            privileged: Run in privileged thread mode.
            stack: The stack pointer the call runs on.
            stack_top: Initial value of that stack pointer, the current one when None.
            interrupts: Whether interrupts stay enabled during the call.
            reset: True restores the call baseline before the call. The first such call takes
                the baseline from the machine as it is and restores nothing;
                `set_call_baseline(m.snapshot())` chooses one explicitly. A Snapshot restores
                that snapshot before the call.
            abi: Float calling convention, the one of the image holding the function when None.

        Returns:
            The value and the stop, RETURNED when the call returned. A fault or an exhausted
            budget is a stop, not an exception.

        Raises:
            TypeError: The argument count differs from DWARF's, or an argument has no kind.
            ValueError: An argument is outside its parameter's C type.
            SymbolError: `fn` names no symbol.
            NotImplementedError: A parameter is a struct passed by value.
            MachineBusy: A run, step, wait or call is active.
            ModelError: A model failed during the call, or the call touched an unmodelled
                register under strict_peripherals.
        """
        entry = self.address_of(fn)
        signature = self._signature(fn, entry)
        where = signature.name
        if signature.unsupported is not None:
            raise NotImplementedError(signature.unsupported)
        if signature.params is not None and len(signature.params) != len(args):
            raise TypeError(f"{where} takes {len(signature.params)} arguments, {len(args)} given")
        frame = _core.CallFrame()
        frame.entry = entry
        frame.args = [
            argument(
                value,
                None if signature.params is None else signature.params[index],
                f"argument {index} of {where}",
            )
            for index, value in enumerate(args)
        ]
        result_slot = signature.returns if returns is None else slot_for(returns)
        frame.returns = result_slot.kind
        frame.abi = _core.FloatAbi((abi or self._abi_at(entry)).value)
        frame.return_sentinel = self._sentinel
        frame.stack = stack_top
        frame.stack_select = _core.CallStack[stack.value]
        frame.privileged = privileged
        frame.interrupts = interrupts
        if isinstance(reset, Snapshot):
            self.restore(reset)
        elif reset:
            if self._call_baseline is None:
                self._call_baseline = self.snapshot()
            else:
                self.restore(self._call_baseline)
        with self._exclusive():
            self._handle.begin_call(frame)
            try:
                stop = await self._drive(
                    until=None,
                    budget=self._default_budget if budget is None else budget,
                    for_=None,
                    stop_when=None,
                    exact=None,
                )
                if stop.reason is StopReason.FAULT:
                    stop = replace(stop, registers=self._fault_context())
                    self._last_stop = stop
            finally:
                bits = self._handle.end_call()
        value = decode(result_slot, bits) if stop.reason is StopReason.RETURNED else 0
        return CallResult(value, bits, stop)

    def _fault_context(self) -> Mapping[str, int]:
        names = list(self._handle.register_names())
        captured = {
            name: self._handle.read_register(names.index(name))
            for name in _FAULT_CONTEXT
            if name in names
        }
        return types.MappingProxyType(captured)

    def _signature(self, fn: int | str | Symbol, entry: int) -> Signature:
        name = fn if isinstance(fn, str) else fn.name if isinstance(fn, Symbol) else f"{fn:#x}"
        for info in reversed(self._debug_infos()):
            function = info.functions.get(name) if isinstance(fn, str | Symbol) else None
            if function is None:
                function = next((f for f in info.functions.values() if f.address == entry), None)
            if function is not None and function.address == entry:
                return signature_of(info, function)
        return Signature(name, None, I32_SLOT)

    def _abi_at(self, address: int) -> FloatAbi:
        for image in reversed(self._images):
            if any(s.start <= address < s.start + s.memory_size for s in image.segments):
                return image.abi
        return FloatAbi.SOFT

    def _type_view(self, debug_type: DebugType | str, address: int) -> Var | None:
        for info in reversed(self._debug_infos()):
            if isinstance(debug_type, str):
                type_id = info.types.get(debug_type)
                if type_id is not None:
                    return Var(self._memory, info, type_id, address, debug_type)
                continue
            for type_id, candidate in info.by_id.items():
                if candidate is debug_type:
                    name = next((n for n, t in info.types.items() if t == type_id), "block")
                    return Var(self._memory, info, type_id, address, name)
        return None

    @overload
    def intercept(
        self, fn: int | str | Symbol, handler: None = None, *, once: bool = False
    ) -> Callable[[InterceptHandler], Intercept]: ...
    @overload
    def intercept(
        self, fn: int | str | Symbol, handler: InterceptHandler, *, once: bool = False
    ) -> Intercept: ...
    def intercept(
        self, fn: int | str | Symbol, handler: InterceptHandler | None = None, *, once: bool = False
    ) -> Intercept | Callable[[InterceptHandler], Intercept]:
        """Intercept calls to a function: `handler(call)` returns `call.proceed()`,
        `call.returns(value)` or `call.stop()`. A decorator when `handler` is omitted.

        The intercept takes effect at once, translated code included, and every call it sees
        goes to `intercept_log(fn)`. The handler runs sync at the function's first instruction
        and may read the machine; an exception it raises reaches the run with its traceback.

        Raises:
            SymbolError: `fn` names no symbol.
            MachineRunning: A run is active, outside its handlers.
        """
        if handler is None:
            return lambda given: self._install_intercept(fn, given, once)
        return self._install_intercept(fn, handler, once)

    @contextlib.contextmanager
    def intercepted(
        self, fn: int | str | Symbol, handler: InterceptHandler
    ) -> Generator[Intercept]:
        """Intercept `fn` for the body of the `with` block only."""
        installed = self._install_intercept(fn, handler, False)
        try:
            yield installed
        finally:
            installed.remove()

    def script(self, fn: int | str | Symbol, replies: Sequence[int | float | None]) -> Intercept:
        """Answer the first calls of `fn` with `replies` in order, then run the original.

        A None reply returns from a void function.
        """
        answers = tuple(replies)

        def answer(call: Call) -> Return:
            if call.occurrence < len(answers):
                return call.returns(answers[call.occurrence])
            return call.proceed()

        return self._install_intercept(fn, answer, False)

    def intercept_log(self, fn: int | str | Symbol) -> list[LogEntry]:
        """Every call the intercepts of `fn` saw, oldest first: arguments from DWARF, and the
        value the intercept returned or, for a call that ran the original, the value it
        returned. The log accumulates across intercepts of `fn` until `remove_hooks()`;
        clear the returned list to start over."""
        return self._logs.setdefault(self.address_of(fn), [])

    @overload
    def on_return(
        self, fn: int | str | Symbol, handler: None = None
    ) -> Callable[[ReturnHandler], Intercept]: ...
    @overload
    def on_return(self, fn: int | str | Symbol, handler: ReturnHandler) -> Intercept: ...
    def on_return(
        self, fn: int | str | Symbol, handler: ReturnHandler | None = None
    ) -> Intercept | Callable[[ReturnHandler], Intercept]:
        """Call `handler(returned)` each time `fn` returns, with its entry arguments, its
        return value and the instructions it took, types from DWARF. A decorator when `handler`
        is omitted.

        Raises:
            TypeError: `fn` has no DWARF signature.
        """
        if handler is None:
            return lambda given: self._install_return_hook(fn, given)
        return self._install_return_hook(fn, handler)

    def watch(
        self,
        target: int | str | Symbol,
        size: int = 4,
        kind: WatchKind = WatchKind.WRITE,
        handler: WatchHandler | None = None,
    ) -> Watch:
        """Watch accesses to `size` bytes at an address or symbol, by the CPU and by DMA.

        Without a handler every hit stops the run with WATCHPOINT. A handler returns
        `WatchAction.STOP` or `RESUME`, None meaning RESUME. Every hit goes to `Watch.hits`.
        An address inside a peripheral window that no register covers is not watched yet.

        Raises:
            ConfigError: The range is empty or reaches no memory and no peripheral.
        """
        self._require_stopped()
        address = self.address_of(target)
        watch: Watch

        def native(hit: _core.WatchHit) -> _core.WatchAction:
            seen = WatchHit(
                hit.pc, hit.addr, hit.size, hit.value, hit.write, Initiator(hit.by.value)
            )
            watch.hits.append(seen)
            if handler is None:
                return _core.WatchAction.stop
            with self._as_handler():
                action = handler(seen)
            return _core.WatchAction((WatchAction.RESUME if action is None else action).value)

        keys: list[int] = []
        relay = self._relay(native, keys)
        hook = self._handle.add_watch(address, size, _core.WatchKind(kind.value), relay)
        watch = self._keep(Watch(hook, self._callback_table, keys[0]))
        return watch

    def stub_libc(self) -> list[Intercept]:
        """Replace memcpy, memmove, memset, strlen and the `__aeabi_mem*` helpers the images
        define with Python doing the work through `m.mem`, for fast calls and object mode."""
        stubs: dict[str, InterceptHandler] = {
            "memcpy": self._copy_stub(returns_dest=True),
            "memmove": self._copy_stub(returns_dest=True),
            "memset": self._set_stub(order="dcn", returns_dest=True),
            "strlen": self._strlen_stub,
        }
        for suffix in ("", "4", "8"):
            stubs[f"__aeabi_memcpy{suffix}"] = self._copy_stub(returns_dest=False)
            stubs[f"__aeabi_memmove{suffix}"] = self._copy_stub(returns_dest=False)
            stubs[f"__aeabi_memset{suffix}"] = self._set_stub(order="dnc", returns_dest=False)
            stubs[f"__aeabi_memclr{suffix}"] = self._set_stub(order="dn", returns_dest=False)
        known = self.symbols
        return [
            self._install_intercept(name, stub, False)
            for name, stub in stubs.items()
            if name in known
        ]

    def _words(self, call: Call, count: int) -> list[int]:
        return [int(v) & 0xFFFF_FFFF for v in call.args([ValueKind.I32] * count)]

    def _copy_stub(self, *, returns_dest: bool) -> InterceptHandler:
        def copy(call: Call) -> Return:
            dest, src, n = self._words(call, 3)
            if n:
                self.mem.write(dest, self.mem.read(src, n))
            return call.returns(dest if returns_dest else None)

        return copy

    def _set_stub(self, *, order: str, returns_dest: bool) -> InterceptHandler:
        def fill(call: Call) -> Return:
            words = dict(zip(order, self._words(call, len(order)), strict=True))
            if words["n"]:
                self.mem.write(words["d"], bytes([words.get("c", 0) & 0xFF]) * words["n"])
            return call.returns(words["d"] if returns_dest else None)

        return fill

    def _strlen_stub(self, call: Call) -> Return:
        (text,) = self._words(call, 1)
        length = 0
        while True:
            block = self.mem.read(text + length, 64 - (text + length) % 64)
            end = block.find(b"\0")
            if end >= 0:
                return call.returns(length + end)
            length += len(block)

    def _relay(self, handler: Callable[..., object], keys: list[int]) -> Relay:
        """The native side's callable for `handler`, its key appended to `keys`."""
        key = self._callback_table.add(handler)
        keys.append(key)
        return Relay(self._callback_table, key)

    def _keep[H: (Intercept, Watch)](self, hook: H) -> H:
        self._installed = [h for h in self._installed if h.active]
        self._installed.append(hook)
        return hook

    @contextlib.contextmanager
    def _as_handler(self) -> Generator[None]:
        token = _HANDLER_OF.set(self)
        try:
            yield
        finally:
            _HANDLER_OF.reset(token)

    def _install_intercept(
        self, fn: int | str | Symbol, handler: InterceptHandler, once: bool
    ) -> Intercept:
        self._require_stopped()
        entry = self.address_of(fn)
        signature = self._signature(fn, entry)
        log = self._logs.setdefault(entry, [])

        def native(view: _core.InterceptCall) -> _core.InterceptResult:
            call = Call(self, view, signature)
            with self._as_handler():
                args = call.args_typed() if signature.params is not None else ()
                reply = handler(call)
            record = LogEntry(call.occurrence, args)
            if reply.action == "returns":
                record.value, record.returned = reply.value, True
            log.append(record)
            return reply.native

        keys: list[int] = []
        hooks = [self._handle.add_intercept(entry, self._relay(native, keys), once)]
        if signature.params is not None:
            kinds = [p.kind for p in signature.params]

            def returned(done: _core.ReturnedCall) -> None:
                pending = next((r for r in reversed(log) if not r.returned), None)
                if pending is not None:
                    pending.value = (
                        None if signature.void else decode(signature.returns, done.value)
                    )
                    pending.returned = True

            hooks.append(
                self._handle.add_return_hook(
                    entry, kinds, signature.returns.kind, self._relay(returned, keys)
                )
            )
        return self._keep(Intercept(hooks, entry, log, self._callback_table, keys))

    def _install_return_hook(self, fn: int | str | Symbol, handler: ReturnHandler) -> Intercept:
        self._require_stopped()
        entry = self.address_of(fn)
        signature = self._signature(fn, entry)
        if signature.params is None and signature.unsupported is None:
            raise TypeError(f"{signature.name} has no DWARF signature for on_return")
        params = signature.params or ()

        def native(done: _core.ReturnedCall) -> None:
            returned = Returned(
                done.function,
                done.occurrence,
                tuple(decode(p, b) for p, b in zip(params, done.args, strict=True)),
                None if signature.void else decode(signature.returns, done.value),
                done.instructions,
            )
            with self._as_handler():
                handler(returned)

        keys: list[int] = []
        hook = self._handle.add_return_hook(
            entry, [p.kind for p in params], signature.returns.kind, self._relay(native, keys)
        )
        return self._keep(Intercept([hook], entry, [], self._callback_table, keys))

    def snapshot(self) -> Snapshot:
        """The whole machine state, restorable with `restore`, saved with `Snapshot.save`.

        Raises:
            MachineRunning: A run is active, handlers of it included.
        """
        self._require_idle()
        offset = 0 if self._arena is None else self._arena.used
        return Snapshot(self._handle.snapshot(), offset)

    def restore(self, snapshot: Snapshot) -> None:
        """Restore a snapshot this machine took, or one read back from a file. The call arena
        and every model restore with it.

        Raises:
            ConfigError: The snapshot was taken by another machine, or a snapshot read back from
                a file names another description, images or configuration, keyed by the digest
                that differs.
            MachineRunning: A run is active, handlers of it included.

        UART frames the firmware transmitted and Python did not read yet are dropped, as are
        bytes waiting for a pty. Host input not yet delivered to the firmware is kept across a
        restore. Bytes already in the UART's receiver return to the snapshot's state. SAI
        frames not read yet and words written to a receiving block and not served yet are
        dropped, and each `AudioStream.dropped` restarts at 0. A USB host returns to the
        snapshot's state, its cable and the control requests it had queued included: results of
        control requests not collected yet are dropped. A CDC stream drops the bytes the device
        sent and Python did not read yet, and gets back the bytes written and not yet taken by
        the device as the snapshot had them. A HID stream likewise drops the input reports not
        read yet and gets back the output reports not yet taken by the device.
        """
        self._require_idle()
        machine_id = snapshot.machine_id
        if machine_id not in (0, self._handle.id()):
            raise ConfigError(
                "the snapshot was taken by another machine: restore into the machine that took"
                " it, or load its saved form",
                file="snapshot",
                key="machine",
                expected="restore into the machine that took it, or load its saved form",
            )
        self._handle.restore(snapshot.native)
        self._last_stop = None
        self._ports.discard()
        self._audio.discard()
        self._usb.discard()
        if self._arena is not None:
            self._arena.reset(snapshot.arena_offset)

    def load_snapshot(self, path: str | os.PathLike[str]) -> Snapshot:
        """Read a saved snapshot and restore it.

        Raises:
            ConfigError: The file is malformed, or names another description, images or
                configuration than this machine's, keyed by the digest that differs.
            MachineRunning: A run is active.
        """
        snapshot = Snapshot.load(path)
        self.restore(snapshot)
        return snapshot

    def locate(self, address: int) -> Location:
        """Symbol, offset, file and line of an address, the rule stops use."""
        return location_of(self._handle.locate(address))

    def address_of(self, target: int | str | Symbol) -> int:
        """Address of a target: an address as is, a symbol, or a symbol name.

        A name is looked up in the loaded images, the last loaded first.

        Raises:
            SymbolError: No loaded image has the name.
        """
        if isinstance(target, int):
            return target
        if isinstance(target, Symbol):
            return target.value
        missing: SymbolError | None = None
        for image in reversed(self._images):
            try:
                return image.lookup(target).value
            except SymbolError as error:
                missing = missing or error
        raise missing or SymbolError(f"no image loaded to look up {target}", name=target)

    async def run(
        self,
        *,
        until: int | str | Symbol | None = None,
        budget: int | None = None,
        for_: VirtualDuration | None = None,
        stop_when: Callable[[Machine], bool] | None = None,
    ) -> Stop:
        """Run until a stop, one slice at a time, yielding to the event loop between slices.

        Args:
            until: Stop with ADDRESS before executing this address or symbol. A run starting
                at it executes at least one instruction first, so a repeated
                `run(until=hook)` goes on to the next hit, as a debugger's continue. An
                interrupt pending at the start runs its handler first: a handler returning to
                this address stops the run there again.
            budget: Stop with BUDGET at the first block boundary past this many instructions,
                `default_budget` when None.
            for_: Stop with TIME after this much virtual time. An idle core, WFI with nothing
                due, sleeps to the deadline and stops with TIME, not IDLE. The stop lands on the
                first boundary at or after the deadline, so `time` can pass it: by less than one
                instruction from an idle core, and by up to one translated block, under 512
                instruction times, from a core executing code. The time is checked at block
                entry, where the whole block is charged. A store that changes the core clock
                rate charges the rest of its block at the new rate.
            stop_when: Sync predicate evaluated at event boundaries and slice ends, never per
                instruction. Stops with USER when it returns True. An exception it raises
                reaches the caller with its traceback.

        Raises:
            MachineBusy: Another run, step, wait or `running()` is active.
        """
        _check_duration(for_)
        target = None if until is None else self.address_of(until)
        with self._exclusive():
            return await self._drive(
                until=target,
                budget=self._default_budget if budget is None else budget,
                for_=for_,
                stop_when=stop_when,
                exact=None,
            )

    async def step(self, n: int = 1) -> Stop:
        """Execute exactly `n` instructions, a BUDGET stop, or an earlier stop.

        Raises:
            MachineBusy: Another run, step, wait or `running()` is active.
        """
        if n <= 0:
            raise ValueError("step count must be above 0")
        with self._exclusive():
            return await self._drive(until=None, budget=None, for_=None, stop_when=None, exact=n)

    async def _wait_until(self, ready: Callable[[], bool], for_: VirtualDuration | None) -> None:
        stop = await self.wait_for(_Ready(ready), for_=for_)
        if stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
            raise _stop_error(stop, "the run waiting for UART data")

    async def wait_for(self, event: Event, *, for_: VirtualDuration | None = None) -> Stop:
        """Run until `event` is ready, a USER stop, or until `for_` elapses, a TIME stop as late
        as `run(for_=)` allows.

        A `Reached` event stops exactly before its target, an ADDRESS stop, and a target equal to
        the current pc continues as `run(until=)` does. Other events stop at
        the first event boundary or slice end after they became ready. Inside `running()`, the
        background run is not driven twice: this awaits the driver, which checks the event at
        slice boundaries.

        Raises:
            MachineBusy: A run, step or wait is active outside `running()`.
        """
        _check_duration(for_)
        if self._background:
            return await self._await_driver(event, for_)
        if isinstance(event, Reached):
            return await self.run(until=event.target, for_=for_)
        return await self.run(for_=for_, stop_when=event.ready)

    @contextlib.asynccontextmanager
    async def running(self) -> AsyncGenerator[None]:
        """Keep the machine running in a background task while the body awaits other things.

        An IDLE stop waits one wall budget and runs again, the firmware waiting for input. Any
        other stop ends the background run, and leaving the body then raises the error of that
        stop, carrying it: FaultError for FAULT, BudgetExceeded for BUDGET, EmulationError
        otherwise. When the body itself raises, its exception propagates with a note naming the
        background stop. Leaving the body cancels the run at a slice boundary.

        Raises:
            MachineBusy: A run, step or wait is active.
            EmulationError: The background run stopped for a reason other than IDLE.
        """
        with self._exclusive():
            self._background = True
            task = asyncio.create_task(self._drive_background())
            try:
                yield
            except BaseException as error:
                stopped = await self._end_background(task)
                if stopped is not None:
                    error.add_note(f"background run stopped: {stopped}")
                raise
            stopped = await self._end_background(task)
        if stopped is not None:
            raise _stop_error(stopped)

    async def _end_background(self, task: asyncio.Task[Stop | None]) -> Stop | None:
        task.cancel()
        stopped: Stop | None = None
        with contextlib.suppress(asyncio.CancelledError):
            stopped = await task
        self._background = False
        self._release_waiters(None)
        return stopped

    @contextlib.contextmanager
    def _exclusive(self) -> Generator[None]:
        if self._busy:
            raise MachineBusy("a run, step, wait or running() is already active on this machine")
        self._busy = True
        try:
            yield
        finally:
            self._busy = False

    def _require_idle(self) -> None:
        if self._busy:
            raise MachineRunning(
                "the machine is running, handlers included, stop or await it first"
            )

    def _require_owner(self) -> None:
        if self._busy and threading.get_ident() != self._thread:
            raise RuntimeError(
                "a board stimulus from another thread while the machine runs: call it from "
                "the machine's thread"
            )

    def _require_stopped(self) -> None:
        if self._busy and _HANDLER_OF.get() is not self:
            raise MachineRunning("the machine is running, stop or await it first")

    def _handler(self, predicate: Callable[[Machine], bool]) -> Callable[[], bool]:
        def call() -> bool:
            token = _HANDLER_OF.set(self)
            try:
                return bool(predicate(self))
            finally:
                _HANDLER_OF.reset(token)

        return call

    def _slice_args(self) -> _core.SliceArgs:
        args = _core.SliceArgs()
        args.max_instructions = self._slice_instructions
        args.wall_budget_ns = self._slice_wall_ns
        return args

    async def _drive(
        self,
        *,
        until: int | None,
        budget: int | None,
        for_: VirtualDuration | None,
        stop_when: Callable[[Machine], bool] | None,
        exact: int | None,
    ) -> Stop:
        self._last_stop = None
        args = self._slice_args()
        args.until = until
        if for_ is not None:
            args.until_time_ps = self._handle.time_ps() + for_.ps
        predicate = None if stop_when is None else self._handler(stop_when)
        remaining = budget
        left = exact
        executed = 0
        while True:
            if remaining is not None:
                args.budget_left = remaining
            if left is not None:
                args.exact_instructions = left
            args.leave_until = executed == 0
            self._ports.pump()
            outcome = self._handle.run_slice(args, predicate)
            self._ports.pump()
            if outcome.stopped is not None:
                self._last_stop = stop_of(outcome.stopped)
                return self._last_stop
            executed += outcome.executed
            if remaining is not None:
                remaining = max(remaining - outcome.executed, 0)
            if left is not None:
                left = max(left - outcome.executed, 1)
            await asyncio.sleep(0)

    async def _drive_background(self) -> Stop | None:
        self._last_stop = None
        args = self._slice_args()
        while True:
            deadlines = [
                w.deadline_ps
                for w in self._waiters
                if w.deadline_ps is not None and not w.future.done()
            ]
            args.until_time_ps = min(deadlines, default=None)
            self._ports.pump()
            outcome = self._handle.run_slice(args, None)
            self._ports.pump()
            stopped = None if outcome.stopped is None else stop_of(outcome.stopped)
            self._check_waiters()
            if stopped is None or stopped.reason is StopReason.TIME:
                await asyncio.sleep(0)
            elif stopped.reason is StopReason.IDLE:
                await asyncio.sleep(self._slice_wall_ns / 1e9)
            else:
                self._last_stop = stopped
                self._release_waiters(stopped)
                return stopped

    async def _await_driver(self, event: Event, for_: VirtualDuration | None) -> Stop:
        deadline = None if for_ is None else self._handle.time_ps() + for_.ps
        future: asyncio.Future[Stop] = asyncio.get_running_loop().create_future()
        self._waiters.append(_Waiter(event, deadline, future))
        return await future

    def _stop_here(self, reason: StopReason) -> Stop:
        pc = self._handle.pc()
        return Stop(
            reason=reason,
            pc=pc,
            location=location_of(self._handle.locate(pc)),
            instructions=self._handle.instructions(),
            time=VirtualTime(self._handle.time_ps()),
        )

    def _check_waiters(self) -> None:
        now = self._handle.time_ps()
        for waiter in list(self._waiters):
            if waiter.future.done():
                self._waiters.remove(waiter)
                continue
            try:
                if self._handler(waiter.event.ready)():
                    waiter.future.set_result(self._stop_here(StopReason.USER))
                elif waiter.deadline_ps is not None and now >= waiter.deadline_ps:
                    waiter.future.set_result(self._stop_here(StopReason.TIME))
            except Exception as error:
                waiter.future.set_exception(error)
            if waiter.future.done():
                self._waiters.remove(waiter)

    def _release_waiters(self, stopped: Stop | None) -> None:
        waiters, self._waiters = self._waiters, []
        for waiter in waiters:
            if waiter.future.done():
                continue
            if stopped is None:
                waiter.future.cancel()
            else:
                waiter.future.set_result(stopped)

    def __repr__(self) -> str:
        return f"Machine({self._description.part}, images={len(self._images)})"


def _model_makers(
    description: MachineDescription, models: Mapping[str, Callable[[], Model]]
) -> dict[str, object]:
    served = {p.name for p in description.peripherals if p.alias_of is None}
    for name in models:
        if name not in served:
            close = difflib.get_close_matches(name, sorted(served))
            raise ConfigError(
                f"models: {description.part} has no peripheral {name}"
                + (f", close matches: {', '.join(close)}" if close else ""),
                file=description.part,
                key=f"models.{name}",
                expected="a peripheral of the description",
            )
    return {name: functools.partial(build_model, maker) for name, maker in models.items()}


_FAULT_CONTEXT = (
    *(f"r{n}" for n in range(13)),
    "sp",
    "lr",
    "pc",
    "xpsr",
    "msp",
    "psp",
    "primask",
    "basepri",
    "faultmask",
    "control",
)

type AttachedDevice = I2CTarget | SpiTarget | At25sf | Eeprom24xx | TableTarget
"""A device `Machine(devices=)` takes, of `popcorn.board` or `popcorn.devices`."""

_BUS_IPS = {I2CTarget: "I2C", SpiTarget: "SPI"}


def _is_u32(value: object, lowest: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and lowest <= value < 1 << 32


def _checked_audio(
    audio: Mapping[str, Mapping[Block, AudioOptions]],
    sais: Sequence[str],
    aliases: Mapping[str, str],
) -> dict[str, dict[Block, AudioOptions]]:
    """`audio` keyed by the SAI serving each key, checked against the built-in SAIs `sais`."""
    checked: dict[str, dict[Block, AudioOptions]] = {}
    for key, blocks in audio.items():
        name = aliases.get(key, key)
        if name not in sais:
            raise ConfigError(
                f"audio.{key}: an SAI with the built-in model, one of {', '.join(sais) or 'none'}",
                key=f"audio.{key}",
                expected="an SAI with the built-in model",
            )
        if name in checked:
            raise ConfigError(
                f"audio.{key}: {name} is listed twice, once through an alias",
                key=f"audio.{key}",
                expected="each SAI once",
            )
        checked[name] = {}
        for block, options in blocks.items():
            at = f"audio.{key}.{block}"
            if block not in ("A", "B"):
                raise ConfigError(f"{at}: an audio block A or B", key=at, expected="A or B")
            for field, value, lowest in (
                ("decimation", options.decimation, 1),
                ("external_frame_hz", options.external_frame_hz, 0),
            ):
                if not _is_u32(value, lowest):
                    raise ConfigError(
                        f"{at}.{field}: an int from {lowest} to 2**32 - 1, not {value!r}",
                        key=f"{at}.{field}",
                        expected=f"an int from {lowest} to 2**32 - 1",
                    )
            checked[name][block] = options
    return checked


def _checked_devices(
    description: MachineDescription,
    devices: Mapping[str, Sequence[BusDevice]],
    models: Mapping[str, object],
) -> dict[str, tuple[BusDevice, ...]]:
    peripherals = {p.name: p for p in description.peripherals if p.alias_of is None}
    seen: set[int] = set()
    out: dict[str, tuple[BusDevice, ...]] = {}

    def refuse(name: str, expected: str) -> ConfigError:
        return ConfigError(
            f"devices.{name}: {expected}",
            file=description.part,
            key=f"devices.{name}",
            expected=expected,
        )

    for name, targets in devices.items():
        peripheral = peripherals.get(name)
        if peripheral is None:
            raise refuse(name, f"a peripheral of {description.part}")
        if peripheral.ip not in _BUS_IPS.values():
            raise refuse(name, "an I2C or SPI peripheral")
        if name in models:
            raise refuse(name, "a peripheral without a Python model")
        listed: list[BusDevice] = []
        for candidate in cast(Sequence[object], targets):
            if not isinstance(candidate, _core.I2CTargetBase | _core.SpiTargetBase):
                raise refuse(
                    name, "popcorn.board.I2CTarget or SpiTarget or popcorn.devices instances"
                )
            target = candidate
            kind = I2CTarget if isinstance(target, _core.I2CTargetBase) else SpiTarget
            listed.append(target)
            if peripheral.ip != _BUS_IPS[kind]:
                raise refuse(name, f"an {_BUS_IPS[kind]} peripheral for an {kind.__name__}")
            if id(target) in seen:
                raise refuse(name, f"{target!r} listed once")
            seen.add(id(target))
            if serving(target) is not None:
                raise refuse(name, f"{target!r} not serving another live machine")
            cs = cs_of(target) if isinstance(target, _core.SpiTargetBase) else None
            if cs is not None:
                port = f"GPIO{cs[1]}"
                if port not in peripherals:
                    raise refuse(name, f"a chip select on a GPIO port of {description.part}")
        if listed:
            out[name] = tuple(listed)
    return out


def _checked_budget(budget: object) -> int | None:
    if budget is None:
        return None
    if isinstance(budget, bool) or not isinstance(budget, int):
        raise TypeError(f"the default budget is an int or None, not {type(budget).__name__}")
    if budget <= 0:
        raise ValueError(f"the default budget must be above 0, not {budget}")
    return budget


def _free_sentinel(description: MachineDescription) -> int:
    """First 16-byte-aligned address from the default arena base, else from 0x1000, below the
    Arm system region, that no region or peripheral covers: the return address of a call without
    an arena."""
    spans = [(r.start, r.start + r.size) for r in description.regions]
    spans += [(p.base, p.base + p.size) for p in description.peripherals]
    for start in (DEFAULT_ARENA_BASE, 0x1000):
        address = start
        while address < 0xE000_0000:
            covering = next((end for low, end in spans if low <= address < end), None)
            if covering is None:
                return address
            address = (covering + 15) & ~15
    raise ConfigError(
        "no free address below 0xE0000000 for a call return",
        file="machine",
        key="arena",
        expected="an arena or an unmapped address for call returns",
    )
