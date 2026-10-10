"""The emulated machine, run on its own thread and driven from asyncio."""

from __future__ import annotations

import atexit
import contextlib
import contextvars
import dataclasses
import difflib
import functools
import os
import sys
import types
import warnings
from collections.abc import AsyncGenerator, Callable, Generator, Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Any, Protocol, cast, overload, runtime_checkable

from popcorn import _core
from popcorn._arena import Arena
from popcorn._audio import BLOCKS, AudioOptions, AudioPorts, Block
from popcorn._call import (
    F32,
    I32_SLOT,
    I64,
    CallResult,
    HandlerCallWarning,
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
    MachineRunning,
    SymbolError,
)
from popcorn._hooks import (
    CallbackTable,
    Initiator,
    Intercept,
    LogEntry,
    NativeReturns,
    PostedBodies,
    Recording,
    Relay,
    Return,
    ReturnBuffer,
    Returned,
    Watch,
    WatchAction,
    WatchHit,
    WatchKind,
)
from popcorn._host import awaited, watch_logs
from popcorn._image import Image, Location, Symbol, location_of
from popcorn._image_view import ImageView, debug_infos, find_variable
from popcorn._model_build import build_model
from popcorn._nvic import Irqs, Nvic
from popcorn._observe import MachineCoverage, Ranges, Ring, TraceRecording, forward_log, open_trace
from popcorn._rcc import RccOptions
from popcorn._refs import (
    Condition,
    Conditions,
    Pending,
    Readout,
    Ref,
    Refs,
    any_of,
    native_set,
    owner_of,
)
from popcorn._snapshot import Snapshot
from popcorn._stop import Stop, stop_of
from popcorn._streams import Board, Gated, PtyOpener, UartPorts, check_wait, deadline_ps
from popcorn._supply import SupplyOptions
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

_RUNNING = "the machine is running: await m.read() or m.write(), or halt() it first"

atexit.register(_core.shutdown_hosts)


class _ContextCell:
    """The context the Python models and bus devices of a machine run in, a copy of the one
    current at `Machine()`, and the exceptions that ended free runs, by token, each while native
    code holds its end. Native code holds the cell weakly, so a cycle through it is collectable."""

    __slots__ = ("__weakref__", "context", "held")

    def __init__(self, context: contextvars.Context) -> None:
        self.context = context
        self.held: dict[int, BaseException] = {}


def _check_duration(for_: VirtualDuration | None) -> None:
    if for_ is not None and for_.ps < 0:
        raise ValueError(f"for_ must not be negative, got {for_}")


def _check_wait(for_: VirtualDuration | None, deadline: VirtualTime | None) -> None:
    check_wait(for_, deadline)
    _check_duration(for_)


def _ps(value: VirtualDuration | None) -> int | None:
    return None if value is None else value.ps


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

    `ready` is sync and runs on the machine's thread, never per instruction: at slice ends
    inside a free run, `start()` or `running()`, after every event and at slice ends otherwise.
    It may read the machine, which is stopped at an instruction boundary there.
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


@dataclass(slots=True)
class _Ready:
    predicate: Callable[[Machine], bool]
    fired: bool = False
    gate: _core.Predicate | None = None

    def ready(self, machine: Machine) -> bool:
        held = bool(self.predicate(machine))
        self.fired = self.fired or held
        return held


@dataclass(frozen=True, slots=True)
class _Native:
    predicate: _core.Predicate

    def ready(self, machine: Machine) -> bool:
        return self.predicate.holds()


class Machine:
    """One emulated MCU: a description, the images loaded into it and its run state.

    Built powered off: nothing runs until `reset()`. The machine runs on its own thread, started
    by its first run and joined at interpreter exit. `run`, `step` and `call` hand that thread a
    goal and await it, from any event loop. `wait_for` hands it a goal, or a waiter inside a free
    run. `start()` and `halt()`, or `running()`, free-run it while the test awaits or blocks. A
    run serves requests and checks waiters every `slice_instructions` instructions or
    `slice_wall_budget` seconds.

    Parked, with nothing active and nothing started, the sync accessors run on the caller's
    thread: `mem`, `reg`, `periph`, `var`, pins, ADC inputs, `time`, `pc`. While the machine is
    `busy`, each of them raises MachineRunning from every thread but the machine's, reads and
    board stimuli included. `read` and `write` over `ref` access it then: one call is one batch
    at one block boundary, best effort or stamped with `at=`, and two calls may see two machine
    states. `snapshot` and `restore` need it parked, and refuse handlers too.

        r0, word = await m.read(m.ref.reg.r0, m.ref.mem.u32[addr])
        await m.write((m.ref.periph.ADC1.input(5).volts, 1.5), at=deadline)

    Handlers, Python models, bus devices, pin listeners and `stop_when` run on the
    machine's thread holding the GIL and use the sync accessors, mid-slice. One that touches
    another machine raises MachineRunning when another thread holds that machine's lock at that
    moment, even while it is parked. Each callback runs in a copy of the context taken when its
    hook was installed, so what it sets stays in that copy. The Python models and bus devices of
    one machine share one copy, taken at construction.

    A Python callback needs the GIL: while another thread runs Python bytecode, each crossing
    waits up to `sys.getswitchinterval()`, 5 ms by default, see `start`. The machine's thread
    gets its Python thread state at its first callback: a profile or trace function set by
    `threading.setprofile_all_threads` or `settrace_all_threads` before then does not reach it.
    The machine's thread takes the GIL only to run them: a machine without Python models,
    devices or hooks runs, waits and wakes the awaiting loop through the loop's eventfd with no
    GIL, also under a task spinning on `asyncio.sleep(0)`. One with them needs its loop to yield
    for real, an `await` on I/O or an `asyncio.sleep` with a positive delay: a loop spinning on
    `asyncio.sleep(0)` starves its callbacks. A loop without `add_reader` is woken by
    `call_soon_threadsafe`, which takes the GIL on the machine's thread.

    Log records, unmodelled register accesses and model warnings among them, reach the `popcorn`
    logger at WARNING with their virtual time in `popcorn_time`, never on the machine's thread.
    The machine queues them and the awaiting loop forwards them, or the caller at its next
    completed await, `halt`, park or parked access, in order, each flush the records queued when
    it began. A filter or handler that raises stops the flush, and the records after it wait
    for the next one. Its error is raised from `halt` or park, or, for an await or the loop's
    flush, from the machine's next request, `halt` or park, so the awaited result is returned. A
    parked access already took effect, so its flush sends the error to `sys.unraisablehook`.
    Past 4096 queued records the rest are counted and one WARNING reports how many were
    dropped. A machine only reachable through a cycle, an attribute of its Python model or
    device included, is collected parked, running or not: the collection halts a free run and
    sends what ended it by itself to `sys.unraisablehook`, unless a wait already returned or
    raised it. A model, device or hook the machine calls after its `Machine` was found garbage
    is not run, the run stops.

    A handler that blocks forever, on a lock the test holds or an event never set, keeps the
    machine's thread from parking. Ctrl-C then gives the machine up: a cancelled run, step or
    call needs a second Ctrl-C, `halt()` and `running()` exit the first. The call raises
    KeyboardInterrupt, the machine is closed, its pending waits and commands fail at once, every
    later access raises EmulationError, and its thread exits when the handler returns. At
    interpreter exit such a machine is given up after 10 s, or at Ctrl-C, and named on stderr.
    A hook removed from a machine given up, with the callable it holds, stays until the
    machine's thread exits. If the machine is collected before that, it stays until the process
    exits. `del m` of a live machine whose handler blocks waits for the handler: a finalizer
    cannot raise.

    A sync access to a parked machine, `m.mem`, `m.regs` or a view, made from a callback or a
    model of another machine waits while a third thread holds the machine, the GIL released,
    and Ctrl-C ends the wait. Two threads crossing such accesses would deadlock: one of them
    raises MachineRunning at once. A wait the user writes in such a callback, on an Event or a
    lock another thread sets only after its own access, is not seen and blocks like two
    `threading.Lock`s taken in opposite orders.

    A machine does not survive `os.fork`: build machines after the fork, in the process that
    uses them. The child has no machine thread, and the parent's locks may be held, so in the
    child every use of a machine built before the fork raises EmulationError naming it, the
    result of a request made before it too, such a request reads as not done, and the child's
    exit and collections leave them alone. Python warns about a fork of a process with a
    started machine, which has threads. An event loop that awaited a machine owns a waker
    eventfd the child inherits and must not use.

    An exception a callback, or a command of `read`, `write` or a stream, raises on the machine's
    thread leaves its frames there, so one `gc.collect()` once the callback returned collects a
    dropped machine. It keeps its type, message, notes, chained exceptions and formatted
    traceback lines, `tb_lineno` and `tb_lasti`. Each `tb_frame` of it and of the exceptions
    chained to it is a new frame with empty `f_locals`, no `f_back` and an `f_lineno` that is not
    the raise line: `pytest -l` shows no locals there and a `__tracebackhide__` local no longer
    hides the frame. A traceback with a frame still running, the handler of an exception it
    chains, keeps its frames. A callback that catches the error of a nested callback, a model
    its own access reached, gets it detached already. A machine collected with such an error
    unclaimed reports its type and message.
    """

    __slots__ = (
        "__weakref__",
        "_arena",
        "_audio",
        "_board",
        "_call_baseline",
        "_callback_table",
        "_clocked",
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
        "_logs",
        "_memory",
        "_model_context",
        "_nvic",
        "_parked_ends",
        "_peripherals",
        "_ports",
        "_posted_bodies",
        "_refs",
        "_registers",
        "_ring",
        "_sentinel",
        "_surfaced",
        "_usb",
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
        supply: SupplyOptions | None = None,
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
            slice_instructions: Instructions after which a slice of the machine's thread serves
                its requests and checks its waiters.
            slice_wall_budget: Wall seconds after which a slice of the machine's thread serves
                its requests and checks its waiters.
            config_digest: Digest of the configuration, such as a project's digest, that snapshots
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
            supply: Supply voltages of the board, None for its defaults, a 3.0 V VDDA. VDDA is
                the reference of every conversion of the built-in ADC model.

        Raises:
            ConfigError: No arch module implements the core, an option is out of range, a
                models key names no peripheral, a model is refused, or devices are refused:
                a peripheral that is not a built-in bus of their kind or has a Python model,
                a chip select pin the description lacks, a device already serving a live
                machine or listed twice. With `rcc`: an HSE outside 4 to 50 MHz, an LSE of 0 Hz
                or above 1 MHz, an RCC with a Python model or one no built-in model serves.
                With `audio`: a key that is not an SAI with the built-in model, two keys naming
                one SAI, a block other than A or B, a decimation below 1 or a negative frame
                rate, or a `clocked_by` naming no device of `devices`, named as
                `audio.<SAI>.<block>`. With `usb`: a key that is not a USB device
                controller with the built-in model, two keys naming one controller, a bus reset
                or a timeout not above 0, named as `usb.<NAME>`. With `supply`: a VDDA outside 1620
                to 3600 mV.
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
        if supply is not None:
            config.vdda_mv = supply.vdda_mv
        aliases = {p.name: p.alias_of for p in description.peripherals if p.alias_of is not None}
        config.usb_hosts = checked_usb(usb or {}, aliases)
        config.slice_instructions = slice_instructions
        config.slice_wall_ns = max(1, round(slice_wall_budget * 1e9))
        self._description = description
        self._config_digest = config_digest
        self._model_context = _ContextCell(contextvars.copy_context())
        config.models_context = self._model_context
        self._handle = _core.MachineHandle(description, config)
        self._images: list[Image] = []
        self._refs: Refs | None = None
        self._ports = UartPorts(
            self._handle.attach_uart,
            self._handle.uart_names(),
            {p.name: p.alias_of for p in description.peripherals if p.alias_of is not None},
            self._wait_until,
            self._post,
            _pty_opener(self._handle),
        )
        sais = self._handle.audio_names()
        audio_options = _checked_audio(audio or {}, sais, aliases)
        self._clocked = _clock_wiring(audio_options, attached, aliases)
        self._audio = AudioPorts(
            self._handle.attach_audio,
            sais,
            aliases,
            audio_options,
            self._wait_until,
            self._post,
        )
        self._usb = UsbHosts(
            self._handle.attach_usb,
            self._handle.usb_names(),
            aliases,
            self._wait_until,
            self._post,
            self._ports.bridges,
        )
        self._devices = attached
        for targets in attached.values():
            for target in targets:
                attach(target, self, self._device_audio_clock)
        self._board = Board(
            self._ports,
            attached,
            self._usb,
            part=description.part,
            peripherals=(p.name for p in description.peripherals),
        )
        access = ViewAccess(self._handle, self._ports.stream, self._audio.stream, self._usb.host)
        self._memory = Memory(access)
        self._registers = Registers(access, self._handle.register_names())
        self._peripherals = Peripherals(access, description)
        self._nvic = Nvic(self._memory, description.core.num_irqs)
        self._coverage = MachineCoverage(self._handle, lambda: self._images, self._require_stopped)
        self._ring = Ring(self._handle, self.locate, self._require_stopped, self._require_idle)
        self._callback_table = CallbackTable()
        self._posted_bodies = PostedBodies(self._callback_table)
        self._handle.forward_logs(functools.partial(forward_log, description.part))
        watch_logs(self._handle.log_token(), self)
        self._irqs = Irqs(description)
        self._arena = (
            None
            if arena_base is None
            else Arena(self._memory, arena_base, arena_size, self._type_view)
        )
        self._call_baseline: Snapshot | None = None
        self._last_stop: Stop | None = None
        self._surfaced: Stop | BaseException | None = None
        self._parked_ends: list[BaseException] = []
        self._default_budget: int | None = None
        self._logs: dict[int, list[LogEntry]] = {}
        self._installed: list[Intercept | Watch] = []
        self._sentinel = (
            self._arena.sentinel if self._arena is not None else _free_sentinel(description)
        )
        for one in (image,) if isinstance(image, Image) else image:
            self.load(one)

    def __del__(self) -> None:
        """Refuse every later callback of the machine and park it before a collection clears it.

        The cycle collector runs every finalizer of a garbage set before it clears any object of
        it. From here on its Python models, bus devices and hooks are never called, so none runs
        on a cleared object. A machine a finalizer resurrects keeps refusing them, and the first
        refused call stops its run. What ended a free run by itself goes to
        `sys.unraisablehook`, unless a wait already returned or raised it, or the collection
        itself caused it. Skipped at interpreter shutdown, and the park on the machine's own
        thread. The frame drops the machine, its handle, its hooks and the end before the report:
        the traceback of the report would otherwise resurrect the machine, or form a cycle with
        the end.
        """
        if sys.is_finalizing():
            return
        handle = getattr(self, "_handle", None)
        table = getattr(self, "_callback_table", None)
        del self
        if table is not None:
            table.close()
        del table
        if handle is None:
            return
        handle.mark_collected()
        if handle.owns_engine():
            return
        try:
            unclaimed = handle.unreported_end()
        except Exception:
            return
        finally:
            del handle
        if unclaimed is not None:
            try:
                raise unclaimed
            finally:
                del unclaimed

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
        """Whether a run, step, call or wait is active, or `start()` was called and `halt()`
        not yet, even after the free run ended by itself."""
        return self._handle.busy()

    @property
    def instructions(self) -> int:
        """Instructions executed since reset.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: use
                `m.read(m.ref.instructions)`.
        """
        return self._handle.instructions()

    @property
    def time(self) -> VirtualTime:
        """Virtual time since reset. A handler, a Python model, a sink or a predicate reads the
        time inside the slice.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: `await m.now()`
                gives the time of the next block boundary, and the `.time` of any `m.read()`
                result is the time of the boundary it executed at.
        """
        return VirtualTime(self._handle.time_ps())

    @property
    def pc(self) -> int:
        """Address of the next instruction. From a handler it is where the core is paused.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: use
                `m.read(m.ref.pc)`.
        """
        return self._handle.pc()

    @property
    def pending_events(self) -> int:
        """Events waiting in the event queue, model and core events alike.

        A model event superseded without being cancelled counts until it is dispatched, so a
        count that grows with guest work shows a model leaving stale events behind.

        Raises:
            MachineRunning: The machine is busy, read outside its callbacks: use
                `m.read(m.ref.pending_events)`.
        """
        return self._handle.pending_events()

    def load(self, image: Image) -> None:
        """Load one more image over memory, such as an application after a bootloader.

        Raises:
            ImageError: A segment falls outside the description's memory regions.
            MachineRunning: The machine is busy, outside its handlers.
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
            MachineRunning: The machine is busy, outside its handlers.
        """
        self._require_stopped()
        self._handle.reset(_core.ResetKind(kind.value))
        self._last_stop = None

    @property
    def mem(self) -> Memory:
        """Guest memory, typed accessors included: `m.mem.u32[addr] = 5`.

        Reads and writes raise MachineRunning while the machine is busy, outside its callbacks:
        `m.read(m.ref.mem.u32[addr])` and `m.write` access it then.
        """
        return self._memory

    @property
    def reg(self) -> Registers:
        """Core registers as typed attributes, `m.reg.r0`, or by `popcorn.arm.Reg`.

        Reads and writes raise MachineRunning while the machine is busy, outside its callbacks:
        `m.read(m.ref.reg.r0)` and `m.write` access them then.
        """
        return self._registers

    @property
    def ref(self) -> Refs:
        """References to the machine's state for `read` and `write`, parallel to `mem`, `reg`,
        `periph` and `var`: `m.ref.reg.r0`, `m.ref.mem.u32[addr]`,
        `m.ref.periph.GPIOA.pin(0).level`. Building one never touches the machine."""
        if self._refs is None:
            handle = self._handle
            scalars: dict[str, Callable[[], int]] = {
                "pc": handle.pc,
                "instructions": handle.instructions,
                "pending_events": handle.pending_events,
            }
            self._refs = Refs(
                handle,
                self._memory,
                self._registers,
                self._peripherals,
                scalars.__getitem__,
                self.var,
            )
        return self._refs

    @overload
    def read[A](self, a: Ref[A], /, *, at: VirtualTime | None = None) -> Pending[Readout[A]]: ...
    @overload
    def read[A, B](
        self, a: Ref[A], b: Ref[B], /, *, at: VirtualTime | None = None
    ) -> Pending[Readout[A, B]]: ...
    @overload
    def read[A, B, C](
        self, a: Ref[A], b: Ref[B], c: Ref[C], /, *, at: VirtualTime | None = None
    ) -> Pending[Readout[A, B, C]]: ...
    @overload
    def read[A, B, C, D](
        self, a: Ref[A], b: Ref[B], c: Ref[C], d: Ref[D], /, *, at: VirtualTime | None = None
    ) -> Pending[Readout[A, B, C, D]]: ...
    @overload
    def read[A, B, C, D, E](
        self,
        a: Ref[A],
        b: Ref[B],
        c: Ref[C],
        d: Ref[D],
        e: Ref[E],
        /,
        *,
        at: VirtualTime | None = None,
    ) -> Pending[Readout[A, B, C, D, E]]: ...
    @overload
    def read[A, B, C, D, E, F](
        self,
        a: Ref[A],
        b: Ref[B],
        c: Ref[C],
        d: Ref[D],
        e: Ref[E],
        f: Ref[F],
        /,
        *,
        at: VirtualTime | None = None,
    ) -> Pending[Readout[A, B, C, D, E, F]]: ...
    @overload
    def read[A, B, C, D, E, F, G](
        self,
        a: Ref[A],
        b: Ref[B],
        c: Ref[C],
        d: Ref[D],
        e: Ref[E],
        f: Ref[F],
        g: Ref[G],
        /,
        *,
        at: VirtualTime | None = None,
    ) -> Pending[Readout[A, B, C, D, E, F, G]]: ...
    @overload
    def read[A, B, C, D, E, F, G, H](
        self,
        a: Ref[A],
        b: Ref[B],
        c: Ref[C],
        d: Ref[D],
        e: Ref[E],
        f: Ref[F],
        g: Ref[G],
        h: Ref[H],
        /,
        *,
        at: VirtualTime | None = None,
    ) -> Pending[Readout[A, B, C, D, E, F, G, H]]: ...
    @overload
    def read(
        self, *refs: Ref[Any], at: VirtualTime | None = None
    ) -> Pending[Readout[*tuple[Any, ...]]]: ...
    def read(
        self, *refs: Ref[Any], at: VirtualTime | None = None
    ) -> Pending[Readout[*tuple[Any, ...]]]:
        """Read every ref at one block boundary, on the machine's thread while it runs, at once
        while it is parked. Posted at the call: await the result for the values.

        `r0, word = await m.read(m.ref.reg.r0, m.ref.mem.u32[a])`. No instruction executes
        between the reads. With no ref it reads only the time, `(await m.read()).time`. With
        `at`, the batch runs at the first block boundary at or after it, exactly at it when the
        core is idle there: on a parked machine it waits for a run that reaches it.

        Returns:
            The values in argument order, with the virtual time they were read at.

        A sync read made from a callback of another machine waits for this machine as the class
        doc says, `read` itself never holds a machine while it waits.

        Raises:
            RuntimeError: Called from a handler or a model, of any machine.
            ValueError: A ref of another machine, or `at` before the time of a parked machine.
                From the await, `at` already past when a running machine takes the batch.
            EmulationError: From the await, the batch was superseded by a reset or a restore
                before `at`, or the machine was closed.
            Exception: The error a log filter or handler raised during an earlier flush, kept
                for this request, see the class doc. Nothing was posted.
        """
        self._refuse_on_host()
        for index, ref in enumerate(refs):
            self._own(ref, index)
        ops = [ref.read_op() for ref in refs]
        notes = [f"reading argument {index}, {ref.label}" for index, ref in enumerate(refs)]

        def convert(raw: object) -> Readout[*tuple[Any, ...]]:
            time_ps, values = cast(tuple[int, list[object]], raw)
            decoded = tuple(ref.decode(value) for ref, value in zip(refs, values, strict=True))
            return Readout(decoded, VirtualTime(time_ps))

        return self._submit_ops(ops, notes, at, convert)

    def write(
        self, *pairs: tuple[Ref[Any], object], at: VirtualTime | None = None
    ) -> Pending[Readout[()]]:
        """Write every pair, in order, at one block boundary, as `read` runs. Posted at the
        call: await the result, whose `.time` is the time the writes executed at.

        `await m.write((m.ref.reg.r0, 5), (m.ref.periph.GPIOA.pin(0).level, True))`. A write
        that fails stops the batch, the writes before it stay applied, and the await raises its
        exception with a note naming the argument. Each value is checked and encoded at the
        call. A register field or a variable bit field is written by reading and writing its
        word in one access on the machine's thread, so the other bits keep what the firmware
        wrote meanwhile. A sync write made from a callback of another machine waits for this
        machine as the class doc says, `write` itself never holds a machine while it waits.

        Raises:
            RuntimeError: Called from a handler or a model, of any machine.
            TypeError: No pair, a pair that is not a (ref, value) tuple, a read-only ref, or a
                value of the wrong type.
            ValueError: A value out of range, an ADC voltage, a scalar that `struct` cannot
                pack or a variable value that does not fit, a ref of another machine, or `at` as
                for `read`. Raised here, not from the await.
            EmulationError: From the await, as for `read`.
            Exception: A kept log filter or handler error, as for `read`.
        """
        self._refuse_on_host()
        if not pairs:
            raise TypeError("write() takes at least one (ref, value) pair")
        ops: list[_core.AccessOp] = []
        notes: list[str] = []
        for index, pair in enumerate(cast(tuple[object, ...], pairs)):
            if not isinstance(pair, tuple) or len(cast(tuple[object, ...], pair)) != 2:
                raise TypeError(f"write() argument {index} is not a (ref, value) pair")
            ref, value = cast(tuple[object, object], pair)
            self._own(ref, index)
            ops.append(cast(Ref[Any], ref).write_op(value))
            notes.append(f"writing argument {index}, {cast(Ref[Any], ref).label}")

        def convert(raw: object) -> Readout[()]:
            time_ps, _ = cast(tuple[int, list[object]], raw)
            return Readout((), VirtualTime(time_ps))

        return self._submit_ops(ops, notes, at, convert)

    def sample[T](self, fn: Callable[[], T], *, at: VirtualTime | None = None) -> Pending[T]:
        """Call `fn` at one block boundary and return what it returns, posted at the call as one
        command: at once on the calling thread while the machine is parked, on the machine's
        thread while it runs, or at the first boundary at or after `at`.

            speaking = await m.sample(lambda: decode_status(m))
            at, level = await m.sample(lambda: (m.time, m.var("level").value))

        `fn` is sync and runs as a handler does: holding the GIL, with the sync accessors
        available, `m.var`, `m.mem`, `m.periph`, `m.time`, and no instruction executes while it
        runs. Keep it read-mostly: a write it makes lands at that boundary, unordered against
        other best-effort commands. It runs in a copy of the context current at the call.

        A slow `fn` stalls the machine for as long as it runs, as a slow handler does, and while
        another thread runs Python it waits for the GIL first, see `start`. Requests from inside
        it, `m.read`, `m.write`, `m.sample`, `wait_for`, stream reads and writes, raise
        RuntimeError, or MachineBusy for `run`, `step` or `wait_for`: it cannot await.

        Returns:
            What `fn` returned.

        Raises:
            RuntimeError: Called from a handler, a model or a posted body, of any machine.
            ValueError: `at` before the time of a parked machine. From the await, `at` already
                past when a running machine takes the command.
            EmulationError: From the await, as for `read`.
            BaseException: From the await, what `fn` raised. Only this command fails: a free run
                continues.
            Exception: A kept log filter or handler error, as for `read`. Nothing was posted.
        """
        self._refuse_on_host()
        returned: list[T] = []

        def body() -> None:
            returned.append(fn())

        bodies = self._posted_bodies
        key = bodies.add(body)
        try:
            future = self._handle.submit(
                Relay(self._callback_table, key), None if at is None else at.ps
            )
        except BaseException:
            bodies.drop(key)
            raise
        bodies.posted(key, future)
        return Pending(future, lambda _: returned[0])

    def _own(self, ref: object, index: int) -> None:
        if not isinstance(ref, Ref):
            raise TypeError(f"argument {index} is not a ref of m.ref")
        if ref.owner is not self._handle:
            raise ValueError(f"argument {index}, {ref.label}, is a ref of another machine")

    def _refuse_on_host(self) -> None:
        if _core.on_host_thread() or _core.host_locks_held() > 0:
            raise RuntimeError(
                "a handler accesses the machine directly, it cannot await a machine request"
            )

    def _submit_ops[R](
        self,
        ops: list[_core.AccessOp],
        notes: list[str],
        at: VirtualTime | None,
        convert: Callable[[object], R],
    ) -> Pending[R]:
        return Pending(self._handle.submit_ops(ops, notes, None if at is None else at.ps), convert)

    def _post(
        self,
        command: _core.AccessOp | None,
        at: VirtualTime | None = None,
        *,
        cancel: bool = True,
        follow: Callable[[tuple[Any, VirtualTime], _core.HostFuture], Any] | None = None,
        dropped: Callable[[_core.HostFuture], None] | None = None,
    ) -> Pending[tuple[Any, VirtualTime]]:
        """Run a native command as one command, from a stream, USB or trace method: its value
        and its time. None runs nothing and returns the time it would have run at."""
        self._refuse_on_host()

        def convert(raw: object) -> tuple[Any, VirtualTime]:
            time_ps, values = cast(tuple[int, list[object]], raw)
            return (values[0] if values else None), VirtualTime(time_ps)

        ops = [] if command is None else [command]
        return Pending(
            self._handle.submit_ops(ops, [""] * len(ops), None if at is None else at.ps),
            convert,
            cancel=cancel,
            follow=follow,
            dropped=dropped,
        )

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
            MachineRunning: The machine is busy, outside its handlers.
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
            MachineRunning: The machine is busy, from its handlers too.
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
            MachineRunning: The machine is busy, outside its handlers.
        """
        self._require_stopped()
        recording = open_trace(self._handle, kinds, path, ranges, functions)
        try:
            yield recording
        except BaseException as error:
            try:
                self._close_trace(recording)
            except Exception as close_error:
                error.add_note(f"closing the trace {recording.path} also failed: {close_error}")
            raise
        self._close_trace(recording)

    def _close_trace(self, recording: TraceRecording) -> None:
        if self.busy and not self._handle.owns_engine():
            self._post(recording.close_command())
            return
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
        """Address of a symbol of the loaded images, the last loaded first. A function's address
        has bit 0 clear, `fn_ptr` gives the value a function pointer holds.

        Raises:
            SymbolError: No loaded image has the name, with close matches.
        """
        return self.address_of(name)

    def fn_ptr(self, function: str | Symbol) -> int:
        """The value a C function pointer to `function` holds: its address with bit 0 set, the
        Thumb state bit every Cortex-M function pointer carries. Pass it to `call` where the
        firmware expects a callback, or write it to a function-pointer variable.

            await m.call("HAL_DMA_RegisterCallback", handle, 0, m.fn_ptr("on_done"))

        Reads the loaded images only, never the machine: works parked or running, from any
        thread. A name is looked up as `sym` does, the last loaded image first.

        A routine defined in assembly without `.type name, %function` is STT_NOTYPE and raises
        TypeError here. The loader leaves such a symbol's value as written, often already odd:
        use `m.sym(name)` for it, with bit 0 as the assembler set it.

        Raises:
            SymbolError: No loaded image has the name, with close matches.
            TypeError: The symbol is not a function, STT_FUNC, or `function` is an address.
        """
        if not isinstance(cast(object, function), str | Symbol):
            raise TypeError(
                f"fn_ptr takes a function name or Symbol, not {function!r}: an address carries"
                " no symbol type"
            )
        symbol = function if isinstance(function, Symbol) else self._symbol(function)
        if not symbol.function:
            raise TypeError(
                f"{symbol.name} is not a function symbol: for a routine without"
                f" `.type {symbol.name}, %function` use m.sym({symbol.name!r})"
            )
        return symbol.value | 1

    def _symbol(self, name: str) -> Symbol:
        missing: SymbolError | None = None
        for image in reversed(self._images):
            try:
                return image.lookup(name)
            except SymbolError as error:
                missing = missing or error
        raise missing or SymbolError(f"no image loaded to look up {name}", name=name)

    @property
    def symbols(self) -> dict[str, Symbol]:
        """Every symbol of the loaded images by name, a later image winning a clash."""
        merged: dict[str, Symbol] = {}
        for image in self._images:
            merged.update((s.name, s) for s in image.symbols)
        return merged

    def var(self, name: str) -> Var:
        """A module-scope variable of the loaded ELF images, typed from their DWARF.

        Its reads and writes follow `mem`: parked or from a callback, and through
        `m.ref.var(name)` while the machine is busy.

        Raises:
            SymbolError: No loaded image declares the variable, with close matches.
        """
        _, info, variable = find_variable(self._images, name)
        return Var(self._memory, info, variable.type, variable.address, name)

    @property
    def image(self) -> ImageView:
        """The loaded ELF files as the firmware starts from them: `m.image.var("gains")[2]`.

        Reads the files, never the machine, so it is readable parked or running, from any
        thread. Right for `const` data and initial values, wrong for anything the firmware
        writes, see `ImageView`.
        """
        return ImageView(self._images)

    @property
    def types(self) -> dict[str, DebugType]:
        """Named types of the loaded ELF images' DWARF, typedefs included, a later image winning
        a clash."""
        merged: dict[str, DebugType] = {}
        for info in self._debug_infos():
            merged.update((name, info.resolve(type_id)) for name, type_id in info.types.items())
        return merged

    def _debug_infos(self) -> list[DebugInfo]:
        return [info for _, info in debug_infos(self._images)]

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
        from_thread: bool | None = None,
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
        again. With `from_thread=True`, that context is the one where the wait ended, not the
        stop the caller saw before the call. An exception that ends the call, a ModelError or one
        a handler raised, restores the same as a return, and the machine runs on from where it
        was. If the restore itself fails after such an exception, its error is raised with that
        exception as `__context__`. After a cancel, or a Ctrl-C in a sync wait for the call, it goes
        to `sys.unraisablehook` instead, with a CancelledError as `__context__`, and the task sees
        CancelledError.

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
            from_thread: True runs the machine first until the core would execute a thread-mode
                instruction: IPSR 0 and no exception about to be taken. Handlers finish,
                tail-chained ones included, as they would before a thread-mode caller on
                hardware. The run counts against `budget`, hits intercepts, watches and
                breakpoints, and advances virtual time. A stop before thread mode ends the call
                unbegun, `began` False, its stop in the result. Thread mode is IPSR 0: a return
                to thread mode with other exceptions still active counts, and so does a thread
                put to sleep on exit, SCR.SLEEPONEXIT. A ConfigError for a stack that does not
                fit can come after the wait ran, the ABI and sentinel are checked before.
                Cancelling the task during the wait parks the machine where the wait reached,
                no call begun. False calls at once wherever the core is. None calls at once and
                warns with `HandlerCallWarning` when a handler is active.

        Returns:
            The value and the stop, RETURNED when the call returned. A fault or an exhausted
            budget is a stop, not an exception. `ipsr` is the exception number when the call
            began, `began` False when a `from_thread=True` wait stopped first.

        Raises:
            TypeError: The argument count differs from DWARF's, an argument has no kind, or
                `from_thread` is not a bool or None.
            ValueError: An argument is outside its parameter's C type.
            SymbolError: `fn` names no symbol.
            NotImplementedError: A parameter is a struct passed by value.
            MachineBusy: A run, step, wait or call is active.
            ModelError: A model failed during the call, or the call touched an unmodelled
                register under strict_peripherals.

        Warns:
            HandlerCallWarning: `from_thread` is None and the call began with a handler active.
        """
        if from_thread is not None and not isinstance(cast(object, from_thread), bool):
            raise TypeError(f"from_thread is True, False or None, not {from_thread!r}")
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

        def prepare() -> None:
            if isinstance(reset, Snapshot):
                self.restore(reset)
            elif reset:
                if self._call_baseline is None:
                    self._call_baseline = self.snapshot()
                else:
                    self.restore(self._call_baseline)

        self._last_stop = None
        outcome = await awaited(
            self._handle.call_goal(
                frame,
                self._default_budget if budget is None else budget,
                prepare if reset is not False else None,
                bool(from_thread),
            ),
            lambda raw: cast(_core.CallOutcome, raw),
        )
        stop = stop_of(outcome.stopped)
        if stop.reason is StopReason.FAULT and outcome.began:
            stop = replace(stop, registers=self._fault_context(outcome.registers))
        if not outcome.began and not stop.detail:
            stop = replace(stop, detail="before thread mode")
        self._last_stop = stop
        bits = outcome.bits
        value = decode(result_slot, bits) if stop.reason is StopReason.RETURNED else 0
        if from_thread is None and outcome.began and outcome.ipsr != 0:
            warnings.warn(
                f"{where} was called with exception {outcome.ipsr} active, which resumes after"
                " it: from_thread=True waits for thread mode, from_thread=False silences this",
                HandlerCallWarning,
                stacklevel=2,
            )
        return CallResult(value, bits, stop, ipsr=outcome.ipsr, began=outcome.began)

    def _fault_context(self, values: Sequence[int]) -> Mapping[str, int]:
        read = dict(zip(self._handle.register_names(), values, strict=True))
        captured = {name: read[name] for name in _FAULT_CONTEXT if name in read}
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
            MachineRunning: The machine is busy, outside its handlers.
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

        A return is the branch to the return address `fn` got at entry. For an exception
        handler, or a function the handler reached by a tail call, that is the exception
        return through EXC_RETURN: `bx lr` or `pop {pc}` in handler mode, through any valid
        EXC_RETURN, so an RTOS context switch that returns another one counts too. It counts
        before the core unstacks, so `value` holds what `fn` left in its result registers, and
        before a tail-chained handler runs. A nested exception preempting `fn` is no return of
        it: frames match on the stack pointer. An exception return that faults while unstacking
        still counts, the branch ran. When a handler raises, the other return hooks completing
        at that same point, on `fn` or on the handler a raising `fn` was tail-called from, are
        not reported and their `occurrence` does not advance.

        Raises:
            TypeError: `fn` has no DWARF signature.
        """
        if handler is None:
            return lambda given: self._install_return_hook(fn, given)
        return self._install_return_hook(fn, handler)

    @overload
    def record_returns(
        self, fn: int | str | Symbol, convert: None = None, *, capacity: int = 65536
    ) -> Recording[Returned]: ...
    @overload
    def record_returns[T](
        self, fn: int | str | Symbol, convert: Callable[[Returned], T], *, capacity: int = 65536
    ) -> Recording[T]: ...
    def record_returns(
        self,
        fn: int | str | Symbol,
        convert: Callable[[Returned], object] | None = None,
        *,
        capacity: int = 65536,
    ) -> Recording[Any]:
        """Record each return of `fn` with its virtual time, `convert(returned)` as the value when
        given, from now until `remove()` or `remove_hooks()`.

            ptt = m.record_returns("set_ptt", lambda r: bool(r.args[0]))
            mark = ptt.mark()
            await m.elapse(ms(50))
            assert [e.value for e in ptt.since(mark)] == [True, False]

        A return is what `on_return` counts, an exception handler's return through EXC_RETURN
        included. A return another hook raised on is not recorded.

        Without `convert`, the machine's thread appends each return natively, with no GIL, and
        `since` decodes the entries it copies. With `convert`, each return takes the GIL and
        `convert` runs on the machine's thread, as an `on_return` handler does: keep it short, a
        slow one stalls the machine, and while another thread runs Python the machine waits for
        the GIL at every return, see `start`. An exception it raises ends the run as a handler's
        does.

        Raises:
            ValueError: `capacity` is not above 0.
            TypeError: `fn` has no DWARF signature.
            MachineRunning: The machine is busy, outside its handlers.
        """
        if capacity <= 0:
            raise ValueError(f"capacity must be above 0, not {capacity}")
        if convert is None:
            log = _core.ReturnLog(capacity)
            native, intercept, decoded = self._add_return_log(fn, log)
            return Recording(NativeReturns(log, decoded), intercept, native.retire)
        buffer = ReturnBuffer(capacity, convert)
        native, intercept = self._add_return_hook(fn, lambda r: buffer.add(self.time, r))
        return Recording(buffer, intercept, native.retire)

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
        return self._add_return_hook(fn, handler)[1]

    def _add_return_hook(
        self, fn: int | str | Symbol, handler: ReturnHandler
    ) -> tuple[_core.Hook, Intercept]:
        entry, kinds, returns, returned_of = self._return_decoder(fn)

        def native(done: _core.ReturnedCall) -> None:
            returned = returned_of(done.occurrence, done.args, done.value, done.instructions)
            with self._as_handler():
                handler(returned)

        keys: list[int] = []
        hook = self._handle.add_return_hook(entry, kinds, returns, self._relay(native, keys))
        return hook, self._keep(Intercept([hook], entry, [], self._callback_table, keys))

    def _add_return_log(
        self, fn: int | str | Symbol, log: _core.ReturnLog
    ) -> tuple[_core.Hook, Intercept, Callable[[_core.LoggedReturn], Returned]]:
        entry, kinds, returns, returned_of = self._return_decoder(fn)
        hook = self._handle.add_return_log(entry, kinds, returns, log)

        def decoded(logged: _core.LoggedReturn) -> Returned:
            return returned_of(logged.occurrence, logged.args, logged.value, logged.instructions)

        return hook, self._keep(Intercept([hook], entry, [], self._callback_table, [])), decoded

    def _return_decoder(self, fn: int | str | Symbol) -> tuple[
        int,
        list[_core.ValueKind],
        _core.ValueKind,
        Callable[[int, Sequence[int], int, int], Returned],
    ]:
        self._require_stopped()
        entry = self.address_of(fn)
        signature = self._signature(fn, entry)
        if signature.params is None and signature.unsupported is None:
            raise TypeError(f"{signature.name} has no DWARF signature for on_return")
        params = signature.params or ()

        def returned_of(
            occurrence: int, args: Sequence[int], value: int, instructions: int
        ) -> Returned:
            return Returned(
                entry,
                occurrence,
                tuple(decode(p, b) for p, b in zip(params, args, strict=True)),
                None if signature.void else decode(signature.returns, value),
                instructions,
            )

        return entry, [p.kind for p in params], signature.returns.kind, returned_of

    def snapshot(self) -> Snapshot:
        """The whole machine state, restorable with `restore`, saved with `Snapshot.save`.

        Raises:
            MachineRunning: The machine is busy, from its handlers too.
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
            MachineRunning: The machine is busy, from its handlers too.

        UART frames the firmware transmitted and Python did not read yet are dropped, as are
        bytes waiting for a pty. Host input not yet delivered to the firmware is kept across a
        restore. Bytes already in the UART's receiver return to the snapshot's state. SAI
        frames not read yet and words written to a receiving block and not served yet are
        dropped, and each `AudioStream.dropped` restarts at 0. A USB host returns to the
        snapshot's state, its cable and the control requests it had queued included: results of
        control requests not collected yet are dropped. A CDC stream drops the bytes the device
        sent and Python did not read yet, and gets back the bytes written and not yet taken by
        the device as the snapshot had them. A HID stream likewise drops the input reports not
        read yet and gets back the output reports not yet taken by the device. A USB audio
        function drops the microphone frames not read yet and gets back the speaker samples not
        yet sent, and its streams run or not as the snapshot had them.
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
            MachineRunning: The machine is busy, from its handlers too.
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
        return self._symbol(target).value

    async def run(
        self,
        *,
        until: int | str | Symbol | None = None,
        budget: int | None = None,
        for_: VirtualDuration | None = None,
        stop_when: Callable[[Machine], bool] | None = None,
    ) -> Stop:
        """Run on the machine's thread until a stop. The event loop is free meanwhile.

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
                instruction, on the machine's thread. Stops with USER when it returns True. An
                exception it raises reaches the caller with its traceback.

        Cancelling the awaiting task halts the run at the next block boundary.

        Raises:
            MachineBusy: Another run, step, call, wait or free run is active.
            Exception: A kept log filter or handler error, as for `read`.
        """
        _check_duration(for_)
        target = None if until is None else self.address_of(until)
        return await self._goal(
            until=target,
            budget=self._default_budget if budget is None else budget,
            for_=for_,
            stop_when=stop_when,
            exact=None,
        )

    async def _bounded_run(
        self,
        *,
        until: int | str | Symbol | None = None,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
        stop_when: Callable[[Machine], bool] | None = None,
    ) -> Stop:
        return await self._goal(
            until=None if until is None else self.address_of(until),
            budget=self._default_budget,
            for_=for_,
            deadline=deadline,
            stop_when=stop_when,
            exact=None,
        )

    async def step(self, n: int = 1) -> Stop:
        """Execute exactly `n` instructions, a BUDGET stop, or an earlier stop.

        Raises:
            MachineBusy: Another run, step, call, wait or free run is active.
        """
        if n <= 0:
            raise ValueError("step count must be above 0")
        return await self._goal(
            until=None, budget=None, for_=None, stop_when=None, exact=n, deadline=None
        )

    async def _goal(
        self,
        *,
        until: int | None,
        budget: int | None,
        for_: VirtualDuration | None,
        stop_when: Callable[[Machine], bool] | None,
        exact: int | None,
        deadline: VirtualTime | None = None,
    ) -> Stop:
        self._last_stop = None
        stop = await awaited(
            self._handle.run_goal(
                until=until,
                budget=budget,
                exact=exact,
                for_ps=_ps(for_),
                deadline_ps=deadline_ps(deadline),
                stop_when=None if stop_when is None else self._handler(stop_when),
            ),
            lambda raw: stop_of(cast(_core.StopInfo, raw)),
        )
        self._last_stop = stop
        return stop

    async def _wait_until(
        self,
        ready: _core.Predicate | Gated,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None = None,
    ) -> VirtualTime:
        if isinstance(ready, Gated):
            return await self._wait_gated(ready, for_, deadline)
        if not self.busy:
            with contextlib.suppress(MachineRunning):
                if ready.holds():
                    return self.time
        if self._handle.session_open():
            raw, ended = cast(
                tuple[_core.StopInfo, bool],
                await awaited(
                    self._handle.wait_predicate_ended(ready, _ps(for_), deadline_ps(deadline)),
                    lambda got: got,
                ),
            )
            stop = stop_of(raw)
            if ended:
                raise _stop_error(stop, f"the free run, while waiting for {ready.what()}")
            return stop.time
        stop = await self.wait_for(_Native(ready), for_=for_, deadline=deadline)
        if stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
            raise _stop_error(stop, f"the run waiting for {ready.what()}")
        return stop.time

    async def _wait_gated(
        self,
        gated: Gated,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
    ) -> VirtualTime:
        ready = gated.ready
        event = _Ready(lambda _: ready(), gate=gated.gate)
        if not self.busy and self._handler(event.ready)():
            return self.time
        stop = await self.wait_for(event, for_=for_, deadline=deadline)
        if stop.reason is StopReason.USER and not event.fired:
            raise _stop_error(stop, "the free run, while waiting for a Python predicate")
        if stop.reason not in (StopReason.USER, StopReason.TIME, StopReason.IDLE):
            raise _stop_error(stop, "the run waiting for a Python predicate")
        return stop.time

    async def wait_until(
        self,
        predicate: Callable[[Machine], bool],
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> VirtualTime | None:
        """Run until `predicate(machine)` holds, or until `for_` elapses, and return the virtual
        time of the boundary where it was first seen to hold, None when it was not.

            held = await m.wait_until(lambda m: m.var("link_up").value == 1, for_=ms(50))

        `predicate` is sync, takes this machine and runs on the machine's thread holding the
        GIL, never per instruction. It may use the sync accessors of the machine it gets,
        `m.var`, `m.mem`, `m.time`. On the machine's thread it must not await or request
        anything of a machine: that raises RuntimeError.

        Parked, it runs at event boundaries and slice ends, as `run(stop_when=)` does, and is
        first called once on the calling thread, where requests are not refused: keep it to
        sync reads. The current time is returned when it already holds there. Inside a free
        run, `start()` or `running()`, it runs at slice ends only, from the end of the slice
        after the one that registered the wait, as `wait_for` says: the time returned is up to
        one slice after it became true, two when it held at the call. `for_` is counted from
        the boundary that registered the wait. Cancelling it while parked halts the run, inside
        a free run it drops the wait only.

        `deadline` stops waiting at that virtual time instead, exclusive with `for_`. Already
        past, it returns at once: parked after one check of `predicate`, inside a free run with
        None, unchecked.

        Returns:
            The time of the first boundary where `predicate` was seen to return True. None when
            `for_` elapsed or `deadline` passed first, or a parked run without either stopped
            with IDLE before it held.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `for_` is negative.
            MachineBusy: A run, step, call or wait is active outside a free run.
            RuntimeError: Called from a handler, a model, a predicate or a posted body.
            EmulationError: The run stopped for another reason: FaultError for FAULT,
                BudgetExceeded for BUDGET, EmulationError otherwise, USER included when an
                intercept's `call.stop()` stopped a parked run. Inside a free run, the stop
                that ended it.
            BaseException: What `predicate` raised, or what a handler of the free run raised.
        """
        self._refuse_on_host()
        _check_wait(for_, deadline)
        event = _Ready(predicate)
        if not self.busy and self._handler(event.ready)():
            return self.time
        stop = await self.wait_for(event, for_=for_, deadline=deadline)
        if stop.reason is StopReason.USER and event.fired:
            return stop.time
        if stop.reason in (StopReason.TIME, StopReason.IDLE):
            return None
        raise _stop_error(stop, "the run waiting for the predicate")

    async def wait_for(
        self,
        event: Event,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> Stop:
        """Run until `event` is ready, a USER stop, or until `for_` elapses, a TIME stop as late
        as `run(for_=)` allows.

        A `Reached` event stops exactly before its target, an ADDRESS stop, and a target equal to
        the current pc continues as `run(until=)` does. Other events stop at the first event
        boundary or slice end after they became ready. Inside a free run, `start()` or
        `running()`, this waits on the free run instead of driving it, checking the event on the
        machine's thread at slice ends only. The wait is registered at a slice boundary after
        the post and first checked at the end of the slice after that one, never at the boundary
        that registered it: the stop is the end of the first slice where the event held, up to
        one slice after it became ready, two when it was ready at the post. With `for_`, posting
        ends the running slice at its next block boundary, which registers the wait and starts
        `for_`, and the event is still checked at the slice end where `for_` elapses: holding
        there, it returns USER. It returns the stop that ended the free run, or raises the
        exception a handler of it raised. Cancelling it then drops the wait only, the machine
        runs on.

        A `Condition`, `m.ref.var("x").ge(3)`, or a `Conditions` from `any_of` or `all_of`, is
        evaluated natively inside a free run, no Python, after every event and at slice ends, so
        it keeps the boundary a parked run sees. When `for_` elapses the wait returns a TIME
        stop without evaluating it there, as a parked run does, unlike a Python event. The
        returned stop's `fired` names the conditions that held at the boundary that ended the
        wait, empty for a stop that is not USER.

        `deadline` stops waiting at that virtual time instead, a TIME stop, exclusive with
        `for_`. Already past, it returns a TIME stop at once: parked, no instruction runs.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `for_` is negative, or a `Condition` or `Conditions` of another machine.
            MachineBusy: A run, step, call or wait is active outside a free run.
        """
        _check_wait(for_, deadline)
        if isinstance(event, Condition | Conditions):
            return await self._wait_condition(event, for_, deadline)
        if self._handle.session_open():
            if isinstance(event, Reached):
                event = _Native(self._handle.predicate_pc_at(self.address_of(event.target)))
            return await self._waited(event, for_, deadline)
        if isinstance(event, Reached):
            return await self._bounded_run(until=event.target, for_=for_, deadline=deadline)
        if isinstance(event, _Native):
            return await self._run_native(event.predicate, for_, deadline)
        if isinstance(event, _Ready) and event.gate is not None:
            return await self._run_gated(event.gate, event.ready, for_, deadline)
        return await self._bounded_run(for_=for_, deadline=deadline, stop_when=event.ready)

    async def _waited(
        self,
        event: Event | None,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None = None,
    ) -> Stop:
        try:
            stop = await awaited(
                self._wait_future(event, _ps(for_), deadline_ps(deadline)),
                lambda raw: stop_of(cast(_core.StopInfo, raw)),
            )
        except BaseException as error:
            self._surfaced = error
            raise
        self._surfaced = stop
        return stop

    def _wait_future(
        self, event: Event | None, for_ps: int | None, deadline_ps: int | None
    ) -> _core.HostFuture:
        if event is None:
            return self._handle.wait(None, for_ps, deadline_ps)
        if isinstance(event, _Native):
            return self._handle.wait_predicate(event.predicate, for_ps, deadline_ps)
        if isinstance(event, _Ready) and event.gate is not None:
            return self._handle.wait_gated(
                event.gate, self._handler(event.ready), for_ps, deadline_ps
            )
        return self._handle.wait(self._handler(event.ready), for_ps, deadline_ps)

    async def elapse(self, duration: VirtualDuration) -> VirtualTime:
        """Let `duration` of virtual time pass, parked or running, and return the time it ended
        at.

        Parked, this runs the machine as `run(for_=duration)` does, `default_budget` included.
        Inside a free run, `start()` or `running()`, it waits on the free run without a
        predicate, `duration` counted from the first block boundary the machine's thread reaches
        after the call. Either way the time returned can pass the deadline by the bound `run`
        gives for its `for_`. A zero duration on a parked machine executes nothing and returns
        the current time. Cancelling it while parked halts the run, inside a free run it drops
        the wait only. Inside a free run, the stop that ended it is held: leaving `running()` or
        calling `halt()` raises it again, as after `wait_for`, also when the body caught the
        error `elapse` raised.

        Returns:
            The virtual time of the TIME stop that ended the wait.

        Raises:
            ValueError: From the await, `duration` is negative.
            MachineBusy: A run, step, call or wait is active outside a free run.
            RuntimeError: From the await, called from a handler, a model, a predicate or a
                posted body.
            EmulationError: The run stopped before the duration passed: FaultError for FAULT,
                BudgetExceeded for BUDGET, EmulationError for another stop. Inside a free run,
                the stop that ended it.
            BaseException: The exception a handler of the free run raised.
            Exception: A kept log filter or handler error, as for `read`.
        """
        self._refuse_on_host()
        _check_duration(duration)
        if self._handle.session_open():
            stop = await self._waited(None, duration)
        else:
            stop = await self.run(for_=duration)
        if stop.reason is not StopReason.TIME:
            raise _stop_error(stop, "the run letting time pass")
        return stop.time

    def now(self) -> Pending[VirtualTime]:
        """The virtual time of the next block boundary, posted at the call as one command: at
        once while the machine is parked, on its thread while it runs. Await it for the time.

        Same as `(await m.read()).time`. Two calls may see two boundaries.

        Raises:
            RuntimeError: Called from a handler, a model or a posted body, of any machine: read
                `m.time` there.
            EmulationError: From the await, the machine was closed.
            Exception: A kept log filter or handler error, as for `read`.
        """
        self._refuse_on_host()
        return self._submit_ops(
            [], [], None, lambda raw: VirtualTime(cast(tuple[int, Any], raw)[0])
        )

    async def _run_native(
        self,
        ready: _core.Predicate,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None = None,
    ) -> Stop:
        self._last_stop = None
        stop = await awaited(
            self._handle.run_predicate(
                ready, self._default_budget, _ps(for_), deadline_ps(deadline)
            ),
            lambda raw: stop_of(cast(_core.StopInfo, raw)),
        )
        self._last_stop = stop
        return stop

    async def _run_gated(
        self,
        gate: _core.Predicate,
        ready: Callable[[Machine], bool],
        for_: VirtualDuration | None,
        deadline: VirtualTime | None,
    ) -> Stop:
        self._last_stop = None
        stop = await awaited(
            self._handle.run_gated(
                gate,
                self._handler(ready),
                self._default_budget,
                _ps(for_),
                deadline_ps(deadline),
            ),
            lambda raw: stop_of(cast(_core.StopInfo, raw)),
        )
        self._last_stop = stop
        return stop

    async def _wait_condition(
        self,
        event: Condition | Conditions,
        for_: VirtualDuration | None,
        deadline: VirtualTime | None = None,
    ) -> Stop:
        if owner_of(event) is not self._handle:
            raise ValueError(f"{event!r} reads another machine")
        conditions = event if isinstance(event, Conditions) else any_of(event)
        members = conditions.members
        if not self._handle.session_open():
            stopped: list[bool] = []

            def stop_when(machine: Machine) -> bool:
                held = event.ready(machine)
                if held:
                    stopped.append(True)
                return held

            stop = await self._bounded_run(for_=for_, deadline=deadline, stop_when=stop_when)
            held = stop.reason is StopReason.USER and bool(stopped)
            fired = tuple(member for member in members if member.ready(self)) if held else ()
            stop = dataclasses.replace(stop, fired=fired)
            self._last_stop = stop
            return stop
        try:
            stop, mask = await awaited(
                self._handle.wait_when(native_set(conditions), _ps(for_), deadline_ps(deadline)),
                _masked_stop,
            )
        except BaseException as error:
            self._surfaced = error
            raise
        fired = tuple(member for index, member in enumerate(members) if mask >> index & 1)
        stop = dataclasses.replace(stop, fired=fired)
        self._surfaced = stop
        return stop

    def start(self) -> None:
        """Run the machine on its own thread until `halt()`, a free run.

        An IDLE stop sleeps until a request or a timer wakes the core. Any other stop, or an
        exception a handler raises, ends the free run and is held: every pending and later
        `wait_for` and stream read returns that stop or raises that exception, and `halt()`
        raises it. The machine stays busy until `halt()`. An end no wait, `halt()` or park
        received is reported to `sys.unraisablehook` when the machine is collected, but dropped
        silently at interpreter exit, which closes machines without their collection.

        A free run advances while the caller blocks: a sync call, a thread join, or a C call that
        releases the GIL. Set up a peer whose connection blocks wall time, a probe or a
        handshake on a pty, while the machine is parked, before `start()` or `running()`.
        A `wait_for` on a Python event sees it at slice ends. A machine built before an
        `os.fork` does not run in the child, see the class.

        Its Python models, handlers and bus devices run slowly while the test thread computes in
        Python: await, block in a C call or sleep instead, or lower `sys.setswitchinterval` in
        the test, 100 us brings a dense callback from 0.3 to 0.7 ms to 12 us. A loop spinning
        on `asyncio.sleep(0)` starves them, see the class: the machine itself and its waits need
        no GIL.

        Raises:
            MachineBusy: A run, step, call, wait or free run is active.
            RuntimeError: Called from a handler, which runs on a machine's thread.
        """
        self._refuse_on_host()
        self._handle.start()
        self._last_stop = None
        self._surfaced = None

    def halt(self) -> None:
        """Stop the free run `start()` started, at its next block boundary, and return once the
        machine is parked. A no-op when nothing was started. A run, step or call active
        instead goes on. The ends parks took and kept, such as the pytest report's park of a
        failed test, are raised by the next `halt()` calls, oldest first, each once.

        Raises:
            MachineBusy: A run, step or call is active instead, cancel its task.
            RuntimeError: Called from a handler.
            EmulationError: The free run stopped by itself: FaultError for FAULT,
                BudgetExceeded for BUDGET, EmulationError otherwise, carrying the stop.
            BaseException: The exception a handler raised during the free run.
        """
        ended = self._halted()
        if isinstance(ended, BaseException):
            raise ended
        if ended is not None:
            raise _stop_error(ended)
        if self._parked_ends:
            raise self._parked_ends.pop(0)

    def _park(self) -> BaseException | None:
        """Bring the machine to parked whatever runs: cancel a goal, halt a free run.

        Returns:
            What ended the free run by itself, as `halt()` would raise it, instead of raising
            it, None when nothing did or when a wait of this free run already returned or
            raised it to its caller. A log filter or handler error the machine kept, which
            `halt()` would raise, is returned the same way and kept as an end.

        Raises:
            KeyboardInterrupt: Ctrl-C while the park waits.
            RuntimeError: Called from a handler.
        """
        self._refuse_on_host()
        try:
            raw = self._handle.park()
        except Exception as kept:
            self._parked_ends.append(kept)
            return kept
        ended = self._ended(raw)
        surfaced, self._surfaced = self._surfaced, None
        if ended is None or ended is surfaced:
            return None
        if isinstance(ended, Stop):
            ended = _stop_error(ended)
        self._parked_ends.append(ended)
        return ended

    def _take_parked_ends(self) -> list[BaseException]:
        """The ends `_park` returned and no `halt()` raised since, oldest first. Clears them."""
        ends, self._parked_ends = self._parked_ends, []
        return ends

    def _flush_logs(self) -> None:
        """Forward the machine's queued log records to `logging`, in order, keeping a filter or
        handler error for the next await, `halt` or park."""
        self._handle.flush_logs()

    def _halted(self) -> Stop | BaseException | None:
        self._refuse_on_host()
        ended = self._ended(self._handle.halt())
        self._surfaced = None
        return ended

    def _ended(self, ended: object) -> Stop | BaseException | None:
        if isinstance(ended, BaseException):
            return ended
        if ended is None:
            return None
        stop = stop_of(cast(_core.StopInfo, ended))
        self._last_stop = stop
        return stop

    @contextlib.asynccontextmanager
    async def running(self) -> AsyncGenerator[None]:
        """Run the machine on its own thread while the body awaits other things: `start()` on
        entry, `halt()` on exit.

        Inside, the machine runs whether the body awaits or blocks. A free run advances while
        the caller blocks: a sync call, a thread join, or a C call that releases the GIL. Set up
        a peer whose connection blocks wall time, a probe or a handshake on a pty, while the
        machine is parked, before `start()` or `running()`. Sync reads, writes and
        stimuli raise MachineRunning: `m.read` and `m.write` access the machine, `wait_for` and
        stream reads wait on the free run. An IDLE stop sleeps until a request wakes the core.

        Any other stop, or an exception a handler raises, an intercept, return hook, watch,
        Python model or bus device, ends the free run at an instruction boundary. Every pending
        and later waiter, `wait_for` or a stream read, returns that stop or raises that
        exception, and leaving the body raises it: FaultError for FAULT, BudgetExceeded for
        BUDGET, EmulationError for another stop, the handler's exception as is. When the body
        itself raises, a cancellation included, the body's exception wins: it propagates with a
        note naming the stop or the handler's exception.

        Raises:
            MachineBusy: A run, step, call, wait or free run is active.
            EmulationError: The free run stopped for a reason other than IDLE.
            BaseException: The exception a handler of the free run raised.
        """
        self.start()
        try:
            yield
        except BaseException as error:
            halting: Exception | None = None
            ended: Stop | BaseException | None = None
            try:
                ended = self._halted()
            except Exception as failed:
                halting = failed
            if halting is not None:
                error.add_note(f"leaving the free run raised: {halting!r}")
            elif isinstance(ended, Stop):
                error.add_note(f"background run stopped: {ended}")
            elif ended is not None and ended is not error:
                error.add_note(f"background run raised: {ended!r}")
            raise
        self.halt()

    def _require_idle(self) -> None:
        if self.busy:
            raise MachineRunning(f"{_RUNNING}, handlers included")

    def _device_audio_clock(self, device: BusDevice, sai: str, block: Block, frame_hz: int) -> None:
        name = self._audio.serving(sai)
        if block not in BLOCKS or self._clocked.get((name, block)) is not device:
            key = f"audio.{name}.{block}.clocked_by"
            raise ConfigError(
                f"{key}: the board does not wire this clock to {device!r}",
                key=key,
                expected=f"the device {device!r}",
            )
        self._audio.stream(name, block).external_frame_hz = frame_hz

    def _require_stopped(self) -> None:
        if self.busy and not self._handle.owns_engine():
            raise MachineRunning(_RUNNING)

    def _handler(self, predicate: Callable[[Machine], bool]) -> Callable[[], bool]:
        def call() -> bool:
            token = _HANDLER_OF.set(self)
            try:
                return bool(predicate(self))
            finally:
                _HANDLER_OF.reset(token)

        return call

    def __repr__(self) -> str:
        return f"Machine({self._description.part}, images={len(self._images)})"


def _masked_stop(raw: object) -> tuple[Stop, int]:
    """The stop and the member mask a native conditions wait resolves to."""
    info, mask = cast(tuple[_core.StopInfo, int], raw)
    return stop_of(info), mask


def _pty_opener(handle: _core.MachineHandle) -> PtyOpener:
    """Open pty bridges on `handle`, holding the handle and not the `Machine`."""

    def opened(name: str, link: _core.UartLink | _core.CdcLink) -> _core.PtyBridge:
        if isinstance(link, _core.CdcLink):
            return handle.open_cdc_pty(name, link)
        return handle.open_uart_pty(name, link)

    return opened


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


def _clock_wiring(
    audio: Mapping[str, Mapping[Block, AudioOptions]],
    attached: Mapping[str, Sequence[BusDevice]],
    aliases: Mapping[str, str],
) -> dict[tuple[str, Block], BusDevice]:
    """The device each `clocked_by` of `audio` names among the `attached` devices, keyed by the
    bus serving them, by SAI and block. A bus alias, such as `SEC_I2C1`, names the bus serving
    it. Of two devices matching, the first attached, as on the bus."""
    wired: dict[tuple[str, Block], BusDevice] = {}
    for name, blocks in audio.items():
        for block, options in blocks.items():
            named = options.clocked_by
            if named is None:
                continue
            at = f"audio.{name}.{block}.clocked_by"
            found = _device_named(named, attached, aliases)
            if found is None:
                raise ConfigError(
                    f'{at}: a device of devices such as "I2C1:0x38" or "SPI1:PA4", '
                    f"not {named!r}",
                    key=at,
                    expected='a bus device attached to the machine, such as "I2C1:0x38"',
                )
            wired[name, block] = found
    return wired


def _device_named(
    named: object, attached: Mapping[str, Sequence[BusDevice]], aliases: Mapping[str, str]
) -> BusDevice | None:
    if not isinstance(named, str):
        return None
    bus, colon, where = named.partition(":")
    if not colon:
        return None
    for device in attached.get(aliases.get(bus, bus), ()):
        if isinstance(device, _core.SpiTargetBase):
            if (cs_of(device) or "SS") == where:
                return device
            continue
        try:
            address = int(where, 0)
        except ValueError:
            return None
        if getattr(device, "address", None) == address:
            return device
    return None


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
