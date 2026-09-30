"""Project files: `popcorn.toml` read into a machine."""

from __future__ import annotations

import functools
import importlib
import inspect
import os
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from popcorn import _core
from popcorn._description import MachineDescription, load_description
from popcorn._devices import I2CTarget, SpiTarget, check_address
from popcorn._enums import StopReason
from popcorn._errors import (
    BudgetExceeded,
    ConfigError,
    EmulationError,
    FaultError,
    SymbolError,
)
from popcorn._image import Image
from popcorn._machine import DEFAULT_ARENA_BASE, DEFAULT_ARENA_SIZE, Machine
from popcorn._stop import Stop
from popcorn.model import Model

type TomlValue = int | float | bool | str | list[TomlValue] | dict[str, TomlValue]
"""A TOML value as tomllib gives it."""


@dataclass(frozen=True, slots=True)
class Device:
    """A device on a bus of `[board.<periph>] devices`."""

    address: int
    model: type
    params: dict[str, TomlValue]


@dataclass(frozen=True, slots=True)
class BoardEntry:
    """`[board.<periph>]`: endpoint, model replacement, devices and model parameters."""

    connect: str | None
    model: type | None
    devices: tuple[Device, ...]
    params: dict[str, TomlValue]


class Project:
    """A project file, `popcorn.toml`, with one of its configs applied.

    Paths in the file are relative to it. Every key is checked on load: an unknown key, a value
    of the wrong type or a missing required key raises ConfigError naming the file, the line
    when known and the dotted key.
    """

    __slots__ = ("_board", "_loaded")

    def __init__(self, loaded: _core.LoadedProject) -> None:
        """Wrap a loaded project. Use Project.load instead."""
        self._loaded = loaded
        self._board: dict[str, BoardEntry] | None = None

    @classmethod
    def load(cls, path: str | os.PathLike[str], config: str | None = None) -> Project:
        """Read a project file, applying `[configs.<config>]` when given.

        Raises:
            ConfigError: The file is unreadable or invalid, or the config does not exist.
        """
        return cls(_core.load_project(os.fspath(path), config))

    @property
    def path(self) -> Path:
        """Absolute path of the project file."""
        return Path(self._loaded.path)

    @property
    def name(self) -> str:
        """`[project] name`."""
        return self._loaded.file.project.name

    @property
    def configs(self) -> tuple[str, ...]:
        """Names of the configs the file declares, sorted."""
        return tuple(self._loaded.configs)

    @property
    def config(self) -> str | None:
        """The applied config, None for none."""
        return self._loaded.config

    @property
    def digest(self) -> str:
        """SHA-256 of the canonical content with the config applied, what snapshots record."""
        return self._loaded.digest

    @property
    def file(self) -> _core.ProjectFile:
        """Every section as read, paths resolved."""
        return self._loaded.file

    def description(self) -> MachineDescription:
        """The MCU description: `[mcu] description` when set, else built for `[mcu] part`.

        Raises:
            ConfigError: The description names another part, or the part is unknown.
        """
        mcu = self._loaded.file.mcu
        if mcu.description is not None:
            description = load_description(mcu.description)
            if description.part != mcu.part:
                raise self._error(
                    "mcu.description", f"the description of {mcu.part}, not {description.part}"
                )
            return description
        from popcorn.stm32 import mcu as stm32_mcu

        svd = None if mcu.svd == "auto" else Path(mcu.svd)
        return stm32_mcu(mcu.part, core=mcu.core, svd=svd)

    def images(self) -> tuple[Image, ...]:
        """The images, in declaration order."""
        loaded: list[Image] = []
        for name in self._loaded.file.image_order:
            entry = self._loaded.file.image[name]
            if entry.elf is not None:
                loaded.append(Image.elf(entry.elf))
            elif entry.hex is not None:
                loaded.append(Image.hex(entry.hex))
            elif entry.bin is not None and entry.at is not None:
                loaded.append(Image.bin(entry.bin, entry.at))
        return tuple(loaded)

    def board(self) -> dict[str, BoardEntry]:
        """`[board.<periph>]` with every `module:Class` imported.

        The `[models] python` directories are appended to `sys.path` first, for the life of the
        process: a later import anywhere in the process sees their modules too.

        Raises:
            ConfigError: A module or class is missing, a reference is not a class, a model is
                not a `popcorn.model.Model`, a device is not a `popcorn.board.I2CTarget` or
                `SpiTarget`, an entry has both a model and devices, or a parameter is unknown
                to its class or missing, named as `board.<periph>.<param>`.
        """
        if self._board is None:
            for directory in self._loaded.file.models.python:
                if directory not in sys.path:
                    sys.path.append(directory)
            board: dict[str, BoardEntry] = {}
            for name, entry in self._loaded.file.board.items():
                key = f"board.{name}"
                devices: list[Device] = []
                for index, device in enumerate(entry.devices):
                    at = f"{key}.devices[{index}]"
                    cls = self._resolve(device.model, f"{at}.model")
                    if not issubclass(cls, I2CTarget | SpiTarget):
                        raise self._error(
                            f"{at}.model",
                            f"a popcorn.board.I2CTarget or SpiTarget subclass, not {device.model}",
                        )
                    params = _params(device.params)
                    self._check_params(cls, (device.address,), params, at)
                    devices.append(Device(device.address, cls, params))
                if entry.model is not None and entry.devices:
                    raise self._error(
                        f"{key}.devices", "no devices on a peripheral that has a Python model"
                    )
                model = None if entry.model is None else self._resolve(entry.model, f"{key}.model")
                params = _params(entry.params)
                if model is not None:
                    if not issubclass(model, Model):
                        raise self._error(
                            f"{key}.model", f"a popcorn.model.Model subclass, not {entry.model}"
                        )
                    self._check_params(model, (), params, key)
                board[name] = BoardEntry(entry.connect, model, tuple(devices), params)
            self._board = board
        return dict(self._board)

    def machine(self, config: str | None = None) -> Machine:
        """Build the machine: description, images in order, clock, vector table, strictness,
        board models, bus devices, `connect = "pty"` bridges, `[boot] memory_init` written. Not
        reset. Each call builds new device instances, so two machines never share one.

        Args:
            config: A config to apply instead of this project's, reloading the file.

        Raises:
            ConfigError: As `load`, `description` and `board`, a memory_init symbol or range is
                invalid, a device sits on a peripheral that is not an I2C bus, a device class
                refuses its address, or `Machine(devices=)` refuses the devices.
            ImageError: An image cannot be read or does not fit the memory map.
            ModelError: A `connect` peripheral is not a UART.
        """
        if config is not None and config != self.config:
            return Project.load(self.path, config).machine()
        file = self._loaded.file
        description = self.description()
        served = {p.name for p in description.peripherals if p.alias_of is None}
        models: dict[str, Callable[[], Model]] = {}
        for name, entry in self.board().items():
            if entry.model is None:
                continue
            if name not in served:
                raise self._error(f"board.{name}", f"a peripheral of {description.part}")
            models[name] = functools.partial(entry.model, **entry.params)
        devices = {
            name: self._devices(name, entry, description)
            for name, entry in self.board().items()
            if entry.devices
        }
        m = Machine(
            description,
            image=self.images(),
            core_hz=file.clock.core_hz or 0,
            cpi=file.clock.cpi,
            strict_peripherals=file.models.strict,
            vector_table=file.boot.vtor,
            config_digest=self.digest,
            arena_base=_arena_base(file.boot.arena_base),
            arena_size=file.boot.arena_size or DEFAULT_ARENA_SIZE,
            models=models,
            devices=devices,
        )
        for name, entry in self.board().items():
            if entry.connect is None:
                continue
            if entry.connect != "pty":
                raise self._error(f"board.{name}.connect", f'"pty", not "{entry.connect}"')
            m.board.pty(name)
        for index, init in enumerate(file.boot.memory_init):
            key = f"boot.memory_init[{index}]"
            try:
                address = m.address_of(init.symbol)
            except SymbolError as error:
                raise self._error(f"{key}.symbol", "a symbol of the loaded images") from error
            try:
                m.mem.write(address, bytes.fromhex(init.bytes))
            except (ConfigError, EmulationError) as error:
                raise self._error(f"{key}.bytes", f"bytes the memory accepts: {error}") from error
        return m

    async def boot(self, machine: Machine) -> Stop:
        """Reset `machine` and run it to `[boot] until` within `[boot] budget`.

        Returns:
            The ADDRESS stop at `until`.

        Raises:
            ConfigError: `[boot] until` is not set or names no symbol.
            FaultError: The firmware faulted before reaching `until`.
            BudgetExceeded: The budget ran out before `until`.
            EmulationError: Any other stop before `until`.
        """
        boot = self._loaded.file.boot
        if boot.until is None:
            raise self._error("boot.until", "a symbol boot runs to")
        try:
            target = machine.address_of(boot.until)
        except SymbolError as error:
            raise self._error("boot.until", "a symbol of the loaded images") from error
        machine.reset()
        stop = await machine.run(until=target, budget=boot.budget)
        if stop.reason is StopReason.ADDRESS and stop.pc == target:
            return stop
        message = f"boot did not reach {boot.until}: {stop}"
        pc, symbol = stop.pc, stop.location.symbol
        if stop.reason is StopReason.FAULT and stop.fault is not None:
            raise FaultError(
                message,
                kind=stop.fault.kind,
                address=stop.fault.address,
                cfsr=stop.fault.cfsr,
                hfsr=stop.fault.hfsr,
                pc=pc,
                symbol=symbol,
                stop=stop,
            )
        if stop.reason is StopReason.BUDGET:
            raise BudgetExceeded(message, budget=boot.budget or 0, pc=pc, symbol=symbol, stop=stop)
        raise EmulationError(message, reason=stop.reason, pc=pc, symbol=symbol, stop=stop)

    def _devices(
        self, name: str, entry: BoardEntry, description: MachineDescription
    ) -> list[I2CTarget | SpiTarget]:
        peripheral = next(
            (p for p in description.peripherals if p.name == name and p.alias_of is None), None
        )
        built: list[I2CTarget | SpiTarget] = []
        for index, device in enumerate(entry.devices):
            at = f"board.{name}.devices[{index}]"
            if peripheral is None or peripheral.ip != "I2C":
                raise self._error(f"{at}.address", f"an I2C bus of {description.part}")
            if not issubclass(device.model, I2CTarget):
                raise self._error(f"{at}.model", "a popcorn.board.I2CTarget subclass on I2C")
            try:
                check_address(device.address)
            except ValueError as error:
                raise self._error(f"{at}.address", str(error)) from error
            try:
                built.append(device.model(device.address, **device.params))
            except (ValueError, TypeError) as error:
                raise self._error(
                    at, f"arguments {device.model.__qualname__} accepts: {error}"
                ) from error
        return built

    def _error(self, key: str, expected: str) -> ConfigError:
        line = self._loaded.line_of(key)
        return ConfigError(
            f"{self.path}:{line}: {key}: expected {expected}",
            file=str(self.path),
            line=line,
            key=key,
            expected=expected,
        )

    def _check_params(
        self, cls: type, args: tuple[object, ...], params: dict[str, TomlValue], key: str
    ) -> None:
        try:
            signature = inspect.signature(cls)
        except TypeError, ValueError:
            return
        try:
            signature.bind(*args, **params)
        except TypeError as error:
            accepted = list(signature.parameters.values())
            named = [
                p.name for p in accepted if p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)
            ]
            if not any(p.kind is p.VAR_KEYWORD for p in accepted):
                for param in params:
                    if param not in named:
                        raise self._error(
                            f"{key}.{param}",
                            f"a parameter of {cls.__qualname__}: {', '.join(named) or 'none'}",
                        ) from error
            for param in accepted[len(args) :]:
                if (
                    param.default is param.empty
                    and param.kind in (param.POSITIONAL_OR_KEYWORD, param.KEYWORD_ONLY)
                    and param.name not in params
                ):
                    raise self._error(
                        f"{key}.{param.name}", f"a value for {cls.__qualname__}({param.name})"
                    ) from error
            raise self._error(key, f"arguments {cls.__qualname__} accepts: {error}") from error

    def _resolve(self, reference: str, key: str) -> type:
        module_name, colon, class_name = reference.partition(":")
        if not colon or not module_name or not class_name:
            raise self._error(key, f"module:Class, not {reference}")
        try:
            module = importlib.import_module(module_name)
        except ImportError as error:
            raise self._error(key, f"an importable module, not {module_name}") from error
        found: object = getattr(module, class_name, None)
        if found is None:
            raise self._error(key, f"a class of {module_name}, not {class_name}")
        if not isinstance(found, type):
            raise self._error(key, f"a class, {reference} is a {type(found).__name__}")
        return found

    def __repr__(self) -> str:
        return f"Project({self.path}, config={self.config})"


def _params(native: dict[str, object]) -> dict[str, TomlValue]:
    return {key: _toml(value) for key, value in native.items()}


def _toml(value: object) -> TomlValue:
    if isinstance(value, bool | int | float | str):
        return value
    if isinstance(value, list):
        return [_toml(item) for item in cast(list[object], value)]
    if isinstance(value, dict):
        return {str(k): _toml(v) for k, v in cast(dict[object, object], value).items()}
    raise TypeError(f"not a TOML value: {value!r}")


def _arena_base(value: object) -> int | None:
    if value is None:
        return DEFAULT_ARENA_BASE
    if isinstance(value, bool):
        return None
    assert isinstance(value, int)
    return value
