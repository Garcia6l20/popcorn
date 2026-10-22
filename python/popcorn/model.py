"""Peripheral behaviour models written in Python.

A model serves the registers it has handlers for, by name. Every other register of the
peripheral keeps its described storage semantics natively, without a Python call:

    class Timer(Model):
        irq: IrqLine
        expirations: int = 0

        @on_write("CR")
        def _cr(self, old: int, new: int) -> None:
            if new & self.fields.CR.EN and not old & self.fields.CR.EN:
                self.schedule(us(self.regs.ARR), self._expire)

        def _expire(self) -> None:
            self.expirations += 1
            self.regs.SR |= self.fields.SR.UIF
            self.irq.raise_()

    m = popcorn.Machine(description, models={"TIM2": Timer})

Handlers are sync and run with the core paused at an instruction boundary. A register answers
to its name and to its aliases, `CR1` and its SVD name `TIM1_CR1` on TIM1, in `on_write`,
`on_read`, `self.regs` and `self.fields`.
"""

from __future__ import annotations

import functools
import types
import typing
import weakref
import zlib
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from typing import ClassVar

from popcorn import _core
from popcorn._enums import ResetKind, StopReason
from popcorn._errors import ConfigError, ModelError
from popcorn._model_build import build_model, building, registered_classes
from popcorn._part_description import PartDescription
from popcorn._state import StateReader, declared_fields, encode
from popcorn.units import VirtualDuration, VirtualTime

_READS = "_popcorn_reads"
_WRITES = "_popcorn_writes"


def on_read[F: Callable[..., object]](register: str) -> Callable[[F], F]:
    """Make a method serve guest reads of a register, by the name the description gives it.

    The method takes no argument and returns the register value, or None for a bus error. Bits
    above the register width are dropped.
    """

    def mark(method: F) -> F:
        setattr(method, _READS, (*getattr(method, _READS, ()), register))
        return method

    return mark


def on_write[F: Callable[..., object]](register: str) -> Callable[[F], F]:
    """Make a method serve guest writes of a register, by the name the description gives it.

    The written value is stored before the method runs, which takes the register value before
    and after the write, `(old, new)`, and may store another one through `self.regs`. It returns
    None, or False for a bus error, which puts the old value back.
    """

    def mark(method: F) -> F:
        setattr(method, _WRITES, (*getattr(method, _WRITES, ()), register))
        return method

    return mark


class IrqLine:
    """An interrupt request line of the model's peripheral, level-driven."""

    __slots__ = ("_context", "index")

    def __init__(self, context: _core.ModelContext, index: int) -> None:
        """Wrap line `index` of the peripheral, in description order."""
        self._context = context
        self.index = index

    def raise_(self) -> None:
        """Drive the line active."""
        self._context.set_irq(self.index, True)

    def lower(self) -> None:
        """Drive the line inactive."""
        self._context.set_irq(self.index, False)

    @property
    def level(self) -> bool:
        """Whether the line is active."""
        return self._context.irq_level(self.index)


@dataclass(frozen=True, slots=True)
class _Register:
    name: str
    word: int
    shift: int
    mask: int
    first: int
    end: int
    fields: dict[str, int]


def _registers(description: _core.PeripheralDesc) -> dict[str, _Register]:
    out: dict[str, _Register] = {}
    aliases: dict[str, _Register] = {}
    for reg in description.registers:
        width = max(reg.width, 8)
        out[reg.name] = _Register(
            name=reg.name,
            word=reg.offset & ~3,
            shift=(reg.offset & 3) * 8,
            mask=(1 << width) - 1,
            first=reg.offset,
            end=reg.offset + width // 8,
            fields={f.name: ((1 << f.width) - 1) << f.lsb for f in reg.fields},
        )
        aliases |= dict.fromkeys(reg.aliases or (), out[reg.name])
    return aliases | out


def _names(registers: dict[str, _Register]) -> list[str]:
    return [name for name, reg in registers.items() if reg.name == name]


class RegisterStore:
    """Stored register values of the model's peripheral by name, `self.regs.SR |= 1`.

    Reads and writes the storage the firmware, snapshots and `m.periph` see, without access
    semantics or handlers.
    """

    __slots__ = ("_context", "_registers")

    _context: _core.ModelContext
    _registers: dict[str, _Register]

    def __init__(self, context: _core.ModelContext, registers: dict[str, _Register]) -> None:
        """View the storage of `context`'s peripheral."""
        object.__setattr__(self, "_context", context)
        object.__setattr__(self, "_registers", registers)

    def __getattr__(self, name: str) -> int:
        reg = self._register(name)
        return (self._context.peek(reg.word) >> reg.shift) & reg.mask

    def __setattr__(self, name: str, value: int) -> None:
        reg = self._register(name)
        if not 0 <= value <= reg.mask:
            raise ValueError(f"{value:#x} does not fit register {name}")
        word = self._context.peek(reg.word)
        self._context.poke(reg.word, (word & ~(reg.mask << reg.shift)) | (value << reg.shift))

    def __dir__(self) -> list[str]:
        return _names(self._registers)

    def _register(self, name: str) -> _Register:
        reg = self._registers.get(name)
        if reg is None:
            raise AttributeError(f"the peripheral has no register {name}")
        return reg


class FieldMasks:
    """Field masks by register and field name, in place: `self.fields.CR.EN == 1 << 0`."""

    __slots__ = ("_registers",)

    def __init__(self, registers: dict[str, _Register]) -> None:
        """Masks of the fields of `registers`."""
        self._registers = registers

    def __getattr__(self, name: str) -> RegisterFields:
        reg = self._registers.get(name)
        if reg is None:
            raise AttributeError(f"the peripheral has no register {name}")
        return RegisterFields(name, reg.fields)

    def __dir__(self) -> list[str]:
        return _names(self._registers)


class RegisterFields:
    """Field masks of one register."""

    __slots__ = ("_fields", "_register")

    def __init__(self, register: str, fields: dict[str, int]) -> None:
        """Masks of `register`'s fields by name."""
        self._register = register
        self._fields = fields

    def __getattr__(self, name: str) -> int:
        mask = self._fields.get(name)
        if mask is None:
            raise AttributeError(f"register {self._register} has no field {name}")
        return mask

    def __dir__(self) -> list[str]:
        return [*self._fields]


@dataclass(frozen=True, slots=True)
class _Handler:
    register: str
    name: str
    shift: int
    mask: int
    first: int
    end: int
    method: str


@dataclass(frozen=True, slots=True)
class _Spec:
    state: tuple[tuple[str, type], ...]
    irqs: tuple[str, ...]
    events: dict[int, str]
    methods: dict[types.FunctionType, str]


_SPECS: weakref.WeakKeyDictionary[type, _Spec] = weakref.WeakKeyDictionary()


def _spec_of(cls: type[Model]) -> _Spec:
    spec = _SPECS.get(cls)
    if spec is None:
        spec = _SPECS[cls] = _make_spec(cls)
    return spec


def _make_spec(cls: type[Model]) -> _Spec:
    custom = cls.snapshot is not Model.snapshot and cls.restore is not Model.restore
    state, irqs = declared_fields(cls, Model, custom=custom, other=(IrqLine,))
    events: dict[int, str] = {}
    methods: dict[types.FunctionType, str] = {}
    for klass in cls.__mro__:
        for name, value in vars(klass).items():
            if isinstance(value, types.FunctionType) and value not in methods:
                methods[value] = name
                events.setdefault(zlib.crc32(name.encode()), name)
    return _Spec(state, irqs, events, methods)


class Model(_core.ModelBase):
    """Base of a peripheral model written in Python.

    Built by its machine, from `Machine(models={name: cls})`, a machine description's
    `Periph(model=Use(cls))`, or `register()`: the constructor takes the subclass's own
    parameters only and must call `super().__init__()` first.

    State: the annotated fields of type int, bool, float, bytes, bytearray or str are the model
    state that snapshots save and a reset restores to its value after construction. A model
    with other state overrides `snapshot()` and `restore()`. Fields annotated `IrqLine`
    receive the peripheral's IRQ lines in description order.

    Lifetime: the machine owns its models. A model must never keep a reference to its Machine,
    or to anything holding it, such as a test fixture or a MachineDescription: native code holds the
    model, so such a reference is a cycle the garbage collector cannot see, and the machine is
    never freed. `peer()` reaches the other models of the machine, which covers the legitimate
    need.

    DMA requests: none from Python models in v1. The native STM32 models route theirs to the
    DMA controller model through their peers, the core has no DMA request lines.

    Attributes:
        peripheral: Name of the peripheral the model serves.
        regs: Stored register values by name.
        fields: Field masks by register and field name.
        irqs: IRQ lines of the peripheral, in description order.
    """

    peripheral: str
    regs: RegisterStore
    fields: FieldMasks
    irqs: tuple[IrqLine, ...]
    _context: _core.ModelContext
    _words: dict[int, tuple[_Handler, ...]]
    _initial: bytes | None
    _reads: ClassVar[dict[str, str]] = {}
    _writes: ClassVar[dict[str, str]] = {}

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        reads = dict(cls._reads)
        writes = dict(cls._writes)
        for name, value in vars(cls).items():
            for register in typing.cast(tuple[str, ...], getattr(value, _READS, ())):
                reads[register] = name
            for register in typing.cast(tuple[str, ...], getattr(value, _WRITES, ())):
                writes[register] = name
        cls._reads = reads
        cls._writes = writes

    def __init__(self) -> None:
        """Attach the model to the peripheral its machine is building it for.

        Raises:
            TypeError: The model is not built by a machine.
            ConfigError: The class declares more IrqLine fields than the peripheral has IRQ
                lines.
        """
        context = building.get()
        if context is None:
            raise TypeError(
                f"{type(self).__qualname__} is built by its machine: pass the class to "
                "Machine(models=...), a machine description's Periph(model=) or "
                "popcorn.model.register"
            )
        super().__init__()
        spec = _spec_of(type(self))
        description = context.description()
        registers = _registers(description)
        self._context = context
        self._initial = None
        self.peripheral = description.name
        self.regs = RegisterStore(context, registers)
        self.fields = FieldMasks(registers)
        self.irqs = tuple(IrqLine(context, i) for i in range(context.irq_count()))
        if len(spec.irqs) > len(self.irqs):
            raise ConfigError(
                f"{type(self).__qualname__} declares {len(spec.irqs)} IrqLine fields, "
                f"{self.peripheral} has {len(self.irqs)} IRQ lines",
                key=f"{self.peripheral}.{spec.irqs[len(self.irqs)]}",
                expected=f"at most {len(self.irqs)} IrqLine fields",
            )
        for name, line in zip(spec.irqs, self.irqs, strict=False):
            setattr(self, name, line)
        self._words = self._dispatch(registers)

    @property
    def part(self) -> PartDescription:
        """Description of the whole MCU, a copy."""
        return self._context.part()

    @property
    def strict_peripherals(self) -> bool:
        """Whether the machine runs with strict_peripherals, fixed for the machine's life: a
        model may raise ModelError where it would only warn."""
        return self._context.strict_peripherals()

    def peer(self, peripheral: str) -> Model | None:
        """The Python model of another peripheral of the machine, None when it has none.

        Always None while the machine is still building its models, constructors included.
        """
        found: object = self._context.peer(peripheral)
        return found if isinstance(found, Model) else None

    def request_stop(self, reason: StopReason = StopReason.USER, detail: str = "") -> None:
        """Stop the run at the next block boundary with `reason`, `detail` in the stop.

        A request wins at that boundary over a `stop_when`, a wait's condition, a wall budget
        and a SPIN stop: the run ends with this stop.

        Raises:
            ValueError: `reason` is FAULT, which only the core raises.
        """
        if reason is StopReason.FAULT:
            raise ValueError("a model cannot stop with FAULT")
        self._context.request_stop(_core.StopReason(reason.value), detail)

    def capture_initial_state(self, *, context: _core.ModelContext) -> None:
        """Record the state a reset returns to. Called by the machine after construction.

        Raises:
            TypeError: The model was not built for `context`: its constructor did not call
                `super().__init__()`.
        """
        if getattr(self, "_context", None) is not context:
            raise TypeError(f"{type(self).__qualname__}.__init__ must call super().__init__()")
        self._initial = self.snapshot()

    @property
    def now(self) -> VirtualTime:
        """Virtual time since reset."""
        return VirtualTime(self._context.time_ps())

    def schedule(self, delay: VirtualDuration, callback: Callable[[], object]) -> int:
        """Call a method of this model `delay` from now in virtual time.

        The deadline is a virtual time: a core clock change before it fires, such as the
        firmware reprogramming its clock tree, does not move it.

        Events are saved in snapshots by method name, so `callback` must be a method of this
        model, bound to it.

        Returns:
            A handle for `cancel()`.

        Raises:
            TypeError: `callback` is not a method of this model.
            ValueError: `delay` is negative.
        """
        if delay.ps < 0:
            raise ValueError(f"a delay of {delay} is in the past")
        spec = _spec_of(type(self))
        function = getattr(callback, "__func__", None)
        name = spec.methods.get(function) if isinstance(function, types.FunctionType) else None
        if name is None or getattr(callback, "__self__", None) is not self:
            raise TypeError(f"{callback!r} is not a method of this {type(self).__qualname__}")
        kind = zlib.crc32(name.encode())
        if spec.events[kind] != name:
            raise TypeError(f"{name} and {spec.events[kind]} share an event kind, rename one")
        return self._context.schedule(delay.ps, kind, 0)

    def cancel(self, handle: int) -> None:
        """Cancel a pending event of this model, a no-op when it fired already."""
        self._context.cancel(handle)

    def reset(self, kind: ResetKind) -> None:
        """Bring the model to its reset state: by default the state after construction.

        The machine has already put every register of the peripheral to its reset value.
        """
        if self._initial is not None:
            self.restore(self._initial)

    def on_bus_reset(self) -> None:
        """The peripheral entered a reset its bus reset bit holds, such as its STM32 RCC reset
        bit written 1. By default it calls `reset(ResetKind.SOFTWARE)`, as the built-in models
        reset the same way for both.

        Called on the machine thread, inside the firmware or host access that set the bit, at
        its virtual time. The machine has already put every register this model does not claim
        to its reset value. Until the bit is cleared, the machine ignores every write to the
        peripheral, from the CPU, DMA and debugger alike, and calls no write handler of this
        model. Reads are served as usual. Events this model scheduled stay queued: cancel them
        here when the reset must drop them. Never called from a machine reset.
        """
        self.reset(ResetKind.SOFTWARE)

    def snapshot(self) -> bytes:
        """The model state as bytes: the declared state fields, in declaration order."""
        out = bytearray()
        for name, kind in _spec_of(type(self)).state:
            value: object = getattr(self, name)
            if not isinstance(value, kind):
                raise ModelError(
                    f"{self.peripheral}: state field {name} holds a {type(value).__name__}, "
                    f"not a {kind.__name__}",
                    peripheral=self.peripheral,
                )
            encode(out, value)
        return bytes(out)

    def restore(self, data: bytes) -> None:
        """Set the declared state fields from bytes `snapshot()` returned.

        An override that raises during `Machine.restore` leaves the machine unusable: runs,
        calls and snapshots raise EmulationError until a restore succeeds.
        """
        reader = StateReader(data)
        values = [(name, reader.read(kind)) for name, kind in _spec_of(type(self)).state]
        if not reader.done():
            raise ModelError(
                f"{self.peripheral}: {len(data)} bytes of state do not match "
                f"{type(self).__qualname__}",
                peripheral=self.peripheral,
            )
        for name, value in values:
            setattr(self, name, value)

    def _dispatch(self, registers: dict[str, _Register]) -> dict[int, tuple[_Handler, ...]]:
        words: dict[int, list[_Handler]] = {}
        for table in (type(self)._reads, type(self)._writes):
            for register, method in table.items():
                reg = registers.get(register)
                if reg is not None:
                    words.setdefault(reg.word, []).append(
                        _Handler(
                            register, reg.name, reg.shift, reg.mask, reg.first, reg.end, method
                        )
                    )
        return {word: tuple(handlers) for word, handlers in words.items()}

    def _handlers(self, offset: int, size: int, table: dict[str, str]) -> Iterator[_Handler]:
        for handler in self._words.get(offset & ~3, ()):
            if (
                table.get(handler.register) == handler.method
                and handler.first < offset + size
                and offset < handler.end
            ):
                yield handler

    def _claims(self) -> list[str]:
        return sorted({*type(self)._reads, *type(self)._writes})

    def _reset(self, kind: _core.ResetKind) -> None:
        self.reset(ResetKind(kind.value))

    def _on_bus_reset(self) -> None:
        self.on_bus_reset()

    def _read(self, offset: int, size: int) -> int | None:
        word = self._context.peek(offset & ~3)
        for handler in self._handlers(offset, size, type(self)._reads):
            value: object = getattr(self, handler.method)()
            if value is None:
                return None
            if not isinstance(value, int):
                raise ModelError(
                    f"{self.peripheral}.{handler.name}: read handler {handler.method} "
                    f"returned a {type(value).__name__}, not an int",
                    peripheral=self.peripheral,
                    register=handler.name,
                    offset=offset,
                )
            mask = handler.mask << handler.shift
            word = (word & ~mask) | ((value << handler.shift) & mask)
        return (word >> ((offset & 3) * 8)) & ((1 << (8 * size)) - 1)

    def _write(self, offset: int, size: int, old: int, value: int) -> bool | None:
        at = offset & ~3
        before = self._context.peek(at)
        mask = ((1 << (8 * size)) - 1) << ((offset & 3) * 8)
        after = (before & ~mask) | ((value << ((offset & 3) * 8)) & mask)
        self._context.poke(at, after)
        for handler in self._handlers(offset, size, type(self)._writes):
            done: object = getattr(self, handler.method)(
                (before >> handler.shift) & handler.mask, (after >> handler.shift) & handler.mask
            )
            if done is False:
                self._context.poke(at, before)
                return False
        return None

    def _event(self, kind: int, payload: int) -> None:
        name = _spec_of(type(self)).events.get(kind)
        if name is None:
            raise ModelError(
                f"{self.peripheral}: no method of {type(self).__qualname__} for event kind "
                f"{kind:#010x}",
                peripheral=self.peripheral,
            )
        getattr(self, name)()

    def _save(self) -> bytes:
        data = typing.cast(object, self.snapshot())
        if not isinstance(data, bytes | bytearray | memoryview):
            raise ModelError(
                f"{self!r}: snapshot() returned a {type(data).__name__}, not bytes",
                peripheral=self.peripheral,
            )
        return bytes(typing.cast("bytes | bytearray | memoryview[int]", data))

    def _load(self, data: bytes) -> None:
        self.restore(data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} of {self.peripheral}>"


_REGISTERED: dict[tuple[str, str], Callable[[_core.ModelContext], object]] = {}


def register[M: type[Model]](ip: str, version_glob: str = "*") -> Callable[[M], M]:
    """Serve every peripheral of an IP with a model class, in machines built from now on.

    For vendor packages: `@register("tim", "v2.*")`. The class takes no constructor
    parameters. A later registration of the same IP and glob replaces it, a model wired
    explicitly to a peripheral takes precedence.
    """

    def install(cls: M) -> M:
        if not issubclass(cls, Model):
            raise TypeError(f"{cls!r} is not a popcorn.model.Model subclass")
        _core.register_python_model(ip, version_glob)
        _REGISTERED[(ip, version_glob)] = functools.partial(build_model, cls)
        registered_classes[(ip, version_glob)] = cls
        return cls

    return install


def unregister(ip: str, version_glob: str = "*") -> None:
    """Remove a registration of `register()`, for machines built from now on."""
    _core.unregister_python_model(ip, version_glob)
    _REGISTERED.pop((ip, version_glob), None)
    registered_classes.pop((ip, version_glob), None)


__all__ = [
    "FieldMasks",
    "IrqLine",
    "Model",
    "RegisterFields",
    "RegisterStore",
    "on_read",
    "on_write",
    "register",
    "unregister",
]
