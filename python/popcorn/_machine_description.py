"""Machine descriptions: what a machine is built from and how it boots, as a typed value."""

from __future__ import annotations

import copy
import dataclasses
import functools
import hashlib
import inspect
import json
import math
from collections.abc import Buffer, Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal, Self, cast

from popcorn import _core
from popcorn._audio import AudioOptions, Block
from popcorn._constants import Constants
from popcorn._devices import check_address
from popcorn._enums import StopReason
from popcorn._errors import (
    BudgetExceeded,
    ConfigError,
    EmulationError,
    FaultError,
    MissingInput,
    SymbolError,
    absent,
)
from popcorn._image import Image
from popcorn._machine import (
    DEFAULT_ARENA_BASE,
    DEFAULT_ARENA_SIZE,
    AttachedDevice,
    Machine,
    check_machine_name,
    error_prefix,
)
from popcorn._part_description import PartDescription, load_description
from popcorn._rcc import RccOptions
from popcorn._stop import Stop
from popcorn._supply import SupplyOptions
from popcorn._usb import UsbHostOptions
from popcorn.model import Model
from popcorn.plugin import Plugin, PluginModel, _build_cache, _records
from popcorn.units import VirtualDuration

type Param = (
    bool
    | int
    | float
    | str
    | bytes
    | Path
    | VirtualDuration
    | Sequence[Param]
    | Mapping[str, Param]
    | Mapping[int, Param]
    | None
)
"""A constructor keyword value of a model or device class.

Lists and tuples are kept as tuples, mappings as read-only mappings. A Path must be absolute. The
class receives lists and dicts, new ones for each machine.
"""


class _FrozenMap[K, V](Mapping[K, V]):
    __slots__ = ("_items",)

    def __init__(self, items: Mapping[K, V] | None = None) -> None:
        self._items: dict[K, V] = dict(items or {})

    def __getitem__(self, key: K) -> V:
        return self._items[key]

    def __iter__(self) -> Iterator[K]:
        return iter(self._items)

    def __len__(self) -> int:
        return len(self._items)

    def __hash__(self) -> int:
        return hash(frozenset(self._items.items()))

    def __repr__(self) -> str:
        return repr(self._items)


@dataclass(frozen=True, slots=True)
class Elf:
    """An ELF image, loaded at its segment addresses.

    Attributes:
        path: The file, an absolute path. It is read by `MachineDescription.machine`.
    """

    path: Path

    def load(self) -> Image:
        """Read the file."""
        return Image.elf(self.path)

    @property
    def constants(self) -> Constants:
        """The enumerators and enum types this file's DWARF declares, read from the file at
        each lookup, never from a machine: usable at pytest collection."""
        return Constants(_files=[("path", self.path)])


@dataclass(frozen=True, slots=True)
class Bin:
    """A raw binary image.

    Attributes:
        path: The file, an absolute path. It is read by `MachineDescription.machine`.
        at: Guest address of its first byte.
    """

    path: Path
    at: int

    def load(self) -> Image:
        """Read the file."""
        return Image.bin(self.path, self.at)


@dataclass(frozen=True, slots=True)
class Hex:
    """An Intel HEX image.

    Attributes:
        path: The file, an absolute path. It is read by `MachineDescription.machine`.
    """

    path: Path

    def load(self) -> Image:
        """Read the file."""
        return Image.hex(self.path)


type ImageFile = Elf | Bin | Hex
"""An image file of a machine description."""


@dataclass(frozen=True, slots=True, init=False)
class Use:
    """A model class with its constructor keywords, built anew for each machine.

    `Use(At25sf, jedec=[0x1F, 0x86, 0x01], size=4 << 20)`.

    Attributes:
        cls: The class.
        params: Its keyword arguments.
    """

    cls: type
    params: Mapping[str, Param]

    def __init__(self, cls: type, /, **params: Param) -> None:
        """Keep `cls` and `params`.

        Raises:
            ConfigError: `cls` is not a class, or a parameter is not a `Param`, such as a
                lambda or an open file.
        """
        if not isinstance(cast(object, cls), type):
            raise ConfigError(
                f"cls: expected a class, not {type(cls).__qualname__}",
                key="cls",
                expected="a class",
            )
        object.__setattr__(self, "cls", cls)
        object.__setattr__(self, "params", _frozen_params(params))

    def replace(self, **params: Param) -> Self:
        """A copy with `params` merged over these parameters.

        Raises:
            ConfigError: As the constructor.
        """
        changed = copy.copy(self)
        object.__setattr__(changed, "params", _frozen_params({**self.params, **params}))
        return changed

    def __replace__(self, **params: Param) -> Self:
        """`replace`, for `copy.replace`."""
        return self.replace(**params)


@dataclass(frozen=True, slots=True, init=False)
class Device(Use):
    """A bus device class with its constructor keywords: an I2C target by `address`, an SPI
    device by its chip select `cs`, None for the hardware SS output, or an SPI controller with
    neither, such as `Device(SpiController, sck_hz=1_000_000)`.

    Attributes:
        address: 7-bit address of an I2C target, None for an SPI device or controller.
        cs: Chip select pin of an SPI device, such as "PA4", active low, None for an SPI
            controller.
    """

    address: int | None
    cs: str | None

    def __init__(
        self,
        cls: type,
        /,
        *,
        address: int | None = None,
        cs: str | None = None,
        **params: Param,
    ) -> None:
        """Keep `cls`, the bus address or chip select, and `params`.

        Raises:
            ConfigError: As `Use`.
        """
        Use.__init__(self, cls, **params)
        object.__setattr__(self, "address", address)
        object.__setattr__(self, "cs", cs)

    def replace(self, **params: Param) -> Self:
        """A copy with `params` merged over these parameters, `address` and `cs` replacing the
        bus address and chip select when given.

        Raises:
            ConfigError: As the constructor, or `address` is not an int or None, or `cs` not a
                str or None.
        """
        address = params.pop("address", self.address)
        cs = params.pop("cs", self.cs)
        if address is not None and (isinstance(address, bool) or not isinstance(address, int)):
            raise _plain_error("address", f"an int or None, not {type(address).__qualname__}")
        if cs is not None and not isinstance(cs, str):
            raise _plain_error("cs", f"a str or None, not {type(cs).__qualname__}")
        changed = copy.copy(self)
        object.__setattr__(changed, "params", _frozen_params({**self.params, **params}))
        object.__setattr__(changed, "address", address)
        object.__setattr__(changed, "cs", cs)
        return changed

    def __replace__(self, **params: Param) -> Self:
        """`replace`, for `copy.replace`."""
        return self.replace(**params)


@dataclass(frozen=True, slots=True, kw_only=True)
class Periph:
    """What the board does with one peripheral.

    Attributes:
        model: The board model replacing the built-in model: a `Use` of a `popcorn.model.Model`
            class, or a `popcorn.plugin.PluginModel`, `PLUGIN.model("rng")`, of a plugin the
            machine description lists in `plugins`.
        devices: Devices on the bus of an I2C or SPI peripheral.
        connect: "pty" bridges a UART, or the first CDC ACM function of a USB device controller
            with a `host`, to a pseudo terminal on every machine built.
        audio: What the board wires to each audio block of an SAI with the built-in model.
        host: The virtual host of a USB device controller with the built-in model.
    """

    model: Use | PluginModel | None = None
    devices: Sequence[Device] = ()
    connect: Literal["pty"] | None = None
    audio: Mapping[Block, AudioOptions] = field(default_factory=_FrozenMap[Block, AudioOptions])
    host: UsbHostOptions | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "devices", tuple(self.devices))
        object.__setattr__(self, "audio", _FrozenMap(self.audio))

    def replace(self, **changes: object) -> Periph:
        """A copy with `changes` applied."""
        return dataclasses.replace(self, **changes)


@dataclass(frozen=True, slots=True)
class MemoryInit:
    """Bytes written at a symbol after the images load.

    Attributes:
        symbol: Symbol whose address receives the bytes.
        data: The bytes.
    """

    symbol: str
    data: bytes

    def __post_init__(self) -> None:
        data = cast(object, self.data)
        if not isinstance(data, Buffer):
            raise _plain_error("data", f"bytes, not {type(data).__qualname__}")
        object.__setattr__(self, "data", bytes(data))


@dataclass(frozen=True, slots=True, kw_only=True)
class Boot:
    """How a machine boots and what it holds after its images load.

    Attributes:
        vtor: Vector table reset reads, None for the description's default.
        until: Symbol boot runs to, None to only reset.
        budget: Instructions boot may take, None for no limit.
        memory_init: Memory written after the images load.
        arena_base: First address of the call arena, None for the default, False for none.
        arena_size: Size of the call arena in bytes, None for the default.
    """

    vtor: int | None = None
    until: str | None = None
    budget: int | None = None
    memory_init: Sequence[MemoryInit] = ()
    arena_base: int | Literal[False] | None = None
    arena_size: int | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "memory_init", tuple(self.memory_init))
        if self.budget is not None and self.budget < 0:
            raise _plain_error("boot.budget", f"a budget of 0 or more, not {self.budget}")
        base = self.arena_base
        if base is True or (
            base is not None and base is not False and not 0 <= base <= 0xFFFF_FFFF
        ):
            raise _plain_error(
                "boot.arena_base", "an address from 0 to 0xffffffff, or False for no arena"
            )
        if self.arena_size is not None and self.arena_size <= 0:
            raise _plain_error("boot.arena_size", f"a size above 0, not {self.arena_size}")
        if self.vtor is not None and self.vtor < 0:
            raise _plain_error("boot.vtor", f"an address of 0 or more, not {self.vtor:#x}")

    def replace(self, **changes: object) -> Boot:
        """A copy with `changes` applied."""
        return dataclasses.replace(self, **changes)


@dataclass(frozen=True, slots=True, kw_only=True)
class MachineDescription:
    """What a machine is built from and how it boots.

    A value: every field is data, compared and hashed by content, with a `digest` stable across
    processes. `digest` is the identity of a machine description. Equality is coarser: it follows
    Python, so `True == 1 == 1.0` and two machine descriptions differing only by such a param
    compare equal while their digests differ, as the classes receive different types. Model and
    device classes are held with their parameters and built anew for each machine. Image files are
    read by `machine`, so a machine description can be declared before its firmware exists.

    Attributes:
        part: Part number, such as "STM32U575QGIx".
        images: Image files, loaded in order.
        name: Name for reports.
        core: Core of a multi-core part.
        svd: SVD file, None for the one of the local ST install.
        description: part description JSON replacing the one built from `part` and `svd`. Its
            part must equal `part`.
        boot: How the machine boots.
        core_hz: Core clock at power-on in Hz, None for the description's reset clock.
        cpi: Core cycles per instruction, above 0.
        rcc: Clock sources of the built-in RCC model, None for its defaults.
        supply: Supply voltages, None for the defaults.
        board: What the board does with each peripheral, by name.
        plugins: The native plugins the board's models and devices come from, at most one per
            name. A `PluginModel` on the board and a device class of a plugin's bindings module,
            or derived from one, need their plugin listed here. Each one is loaded by
            `machine`. In `digest` by name and code, a dev plugin by its cache key and an
            installed one by the SHA-256 of its library and bindings module, never by path, and
            left out when empty.
        strict: Whether an unmodelled peripheral access raises ModelError. Not in `digest`.
        missing: What the pytest plugin does with a test whose machine description raises
            MissingInput: "fail" it, or "skip" it with the missing path as the reason. Not in
            `digest`.
        configs: Named variants of this machine description, as `config` selects them. Not in
            `digest`.
    """

    part: str
    images: Sequence[ImageFile]
    name: str = ""
    core: str | None = None
    svd: Path | None = None
    description: Path | None = None
    boot: Boot = Boot()
    core_hz: int | None = None
    cpi: float = 1.0
    rcc: RccOptions | None = None
    supply: SupplyOptions | None = None
    board: Mapping[str, Periph] = field(default_factory=_FrozenMap[str, Periph])
    plugins: Sequence[Plugin] = ()
    strict: bool = False
    missing: Literal["fail", "skip"] = "fail"
    configs: Mapping[str, MachineDescription] = field(
        default_factory=_FrozenMap[str, "MachineDescription"]
    )

    def __post_init__(self) -> None:
        object.__setattr__(self, "images", tuple(self.images))
        object.__setattr__(self, "board", _FrozenMap(self.board))
        object.__setattr__(self, "plugins", tuple(self.plugins))
        object.__setattr__(self, "configs", _FrozenMap(self.configs))
        self._check_kinds()
        self._check_plugins()
        if self.missing not in ("fail", "skip"):
            raise _plain_error("missing", f'"fail" or "skip", not {self.missing!r}')
        if not (math.isfinite(self.cpi) and self.cpi > 0.0):
            raise _plain_error("cpi", "a finite number of cycles per instruction above 0")
        if self.core_hz is not None and self.core_hz <= 0:
            raise _plain_error("core_hz", "a core clock above 0 Hz")
        for name, periph in self.board.items():
            key = f"board.{name}"
            if periph.model is not None and periph.devices:
                raise _plain_error(
                    f"{key}.devices", f"no devices on a peripheral that has a {_kind(periph)} model"
                )
            for index, device in enumerate(periph.devices):
                at = f"{key}.devices[{index}]"
                refused = _device_plugin_refusal(device.cls, self.plugins)
                if refused is not None:
                    raise _plain_error(at, refused)
                if device.address is not None and device.cs is not None:
                    raise _plain_error(
                        f"{at}.cs",
                        "an address for an I2C target or a cs for an SPI device, not both",
                    )
                if device.address is not None:
                    try:
                        check_address(device.address)
                    except ValueError as error:
                        raise _plain_error(f"{at}.address", str(error)) from error

    def _check_kinds(self) -> None:
        for index, image in enumerate(cast(tuple[object, ...], self.images)):
            if not isinstance(image, Elf | Bin | Hex):
                raise _plain_error(
                    f"images[{index}]", f"an Elf, Bin or Hex, not {type(image).__qualname__}"
                )
            self._check_path(f"images[{index}].path", image.path)
        self._check_path("svd", self.svd)
        self._check_path("description", self.description)
        for name, periph in cast(Mapping[object, object], self.board).items():
            key = f"board.{name}"
            if not isinstance(name, str):
                raise _plain_error(key, f"a peripheral name, not {type(name).__qualname__}")
            if not isinstance(periph, Periph):
                raise _plain_error(key, f"a Periph, not {type(periph).__qualname__}")
            model = cast(object, periph.model)
            if model is not None and not isinstance(model, Use | PluginModel):
                raise _plain_error(
                    f"{key}.model", f"a Use or a PluginModel, not {type(model).__qualname__}"
                )
            if isinstance(model, Use):
                self._check_param_paths(f"{key}.model", model.params)
            for index, device in enumerate(cast(tuple[object, ...], periph.devices)):
                if not isinstance(device, Device):
                    raise _plain_error(
                        f"{key}.devices[{index}]", f"a Device, not {type(device).__qualname__}"
                    )
                self._check_param_paths(f"{key}.devices[{index}]", device.params)
        for index, plugin in enumerate(cast(tuple[object, ...], self.plugins)):
            if not isinstance(plugin, Plugin):
                raise _plain_error(
                    f"plugins[{index}]",
                    f"a popcorn.plugin.Plugin, not {type(plugin).__qualname__}",
                )
        for name, config in cast(Mapping[object, object], self.configs).items():
            if not isinstance(config, MachineDescription):
                raise _plain_error(
                    f"configs.{name}", f"a MachineDescription, not {type(config).__qualname__}"
                )

    def _check_plugins(self) -> None:
        names: set[str] = set()
        for index, plugin in enumerate(self.plugins):
            if plugin.name in names:
                raise _plain_error(
                    f"plugins[{index}]", f"one plugin named {plugin.name}, not a second one"
                )
            names.add(plugin.name)
        for name, periph in self.board.items():
            model = periph.model
            if isinstance(model, PluginModel) and model.plugin not in self.plugins:
                raise _plain_error(
                    f"board.{name}.model",
                    f"a model of a plugin in plugins, not of plugin {model.plugin.name}",
                )

    def _check_param_paths(self, key: str, value: object) -> None:
        if isinstance(value, Path):
            self._check_path(key, value)
        elif isinstance(value, tuple):
            for index, item in enumerate(cast(tuple[object, ...], value)):
                self._check_param_paths(f"{key}[{index}]", item)
        elif isinstance(value, Mapping):
            for name, item in cast(Mapping[object, object], value).items():
                self._check_param_paths(f"{key}.{name}", item)

    def _check_path(self, key: str, path: object) -> None:
        if path is None and key in ("svd", "description"):
            return
        if not isinstance(path, Path):
            raise _plain_error(key, f"a Path, not {type(path).__qualname__}")
        if not path.is_absolute():
            raise _plain_error(
                key, f"an absolute path such as Path(__file__).parent / {str(path)!r}, not {path}"
            )

    def replace(self, **changes: object) -> MachineDescription:
        """A copy with `changes` applied, as `dataclasses.replace`."""
        return dataclasses.replace(self, **changes)

    def with_configs(self, **configs: MachineDescription) -> MachineDescription:
        """A copy whose `configs` also holds `configs`, replacing those of the same name."""
        return dataclasses.replace(self, configs={**self.configs, **configs})

    def config(self, name: str | None) -> MachineDescription:
        """The config `name` of `configs`, this machine description for None.

        Raises:
            ConfigError: No config has that name, the known ones named.
        """
        if name is None:
            return self
        found = self.configs.get(name)
        if found is None:
            known = ", ".join(sorted(self.configs)) or "none"
            raise _plain_error(f"configs.{name}", f"a config of the machine description: {known}")
        return found

    @property
    def constants(self) -> Constants:
        """The enumerators and enum types the DWARF of the ELF images declares, the last image
        first, read from the files at each lookup. Reads no SVD, builds no description and
        boots nothing: usable at pytest collection. Bin and HEX images declare nothing."""
        return Constants(
            _files=[
                (f"images[{i}].path", image.path)
                for i, image in enumerate(self.images)
                if isinstance(image, Elf)
            ]
        )

    @property
    def digest(self) -> str:
        """SHA-256 of the canonical content, without `name`, `strict`, `missing` and `configs`,
        and without `plugins` when it is empty.

        Equal across processes: classes as `module:qualname`, paths absolute in POSIX form,
        mappings with sorted keys, plugins by name and code, never by path. It is what
        snapshots record as their config digest.
        """
        text = json.dumps(_canonical(self), sort_keys=True, separators=(",", ":"), allow_nan=False)
        return hashlib.sha256(text.encode()).hexdigest()

    def part_description(self) -> PartDescription:
        """The part description: `description` when set, else the one built for `part`.

        Raises:
            ConfigError: The description names another part, or the part is unknown.
        """
        if self.description is not None:
            description = load_description(self.description)
            if description.part != self.part:
                raise _plain_error(
                    "description", f"the description of {self.part}, not {description.part}"
                )
            return description
        from popcorn.stm32 import mcu as stm32_mcu

        return stm32_mcu(self.part, core=self.core, svd=self.svd)

    def machine(
        self,
        *,
        strict_peripherals: bool | None = None,
        devices: Mapping[str, Sequence[AttachedDevice]] | None = None,
        name: str | None = None,
    ) -> Machine:
        """Build the machine: description, images in order, clock, vector table, strictness,
        plugins loaded in order, board models, bus devices, RCC clock sources, SAI audio blocks,
        USB hosts, supply voltages, `connect="pty"` bridges, `boot.memory_init` written. Not
        reset. Each call builds new model and device instances, so two machines never share
        one.

        `connect="pty"` bridges through `Board.pty`, so the pty owns what the firmware sends:
        reads of that UART's `port`, or of that USB controller's first CDC ACM function through
        `host.cdc()`, raise RuntimeError on every machine built. Leave `connect` out and call
        `m.board.pty(name)` in the tests that want the pty.

        Args:
            strict_peripherals: Whether unmodelled peripheral accesses raise ModelError,
                `strict` when None. Not part of `digest`.
            devices: Bus devices per I2C or SPI peripheral name or alias, as
                `Machine(devices=)` takes them. A listed peripheral gets these devices instead
                of its `board` devices, none for an empty list. A peripheral not listed keeps
                the machine description's. Not part of `digest`.
            name: Name of the machine in its log records and errors, as `Machine(name=)`
                takes it, the part when None. Not part of `digest`.

        Raises:
            ValueError: `name` is empty or holds whitespace or a dot.
            ConfigError: As `part_description`, a model is not a `popcorn.model.Model` or a
                device not a `popcorn.board.I2CTarget`, `SpiTarget` or `SpiController`, a class
                refuses its parameters, a model replaces no peripheral of the description, a
                plugin fails to build or load or its library names another plugin, as
                `plugins[i]`, a `PluginModel` names no model its plugin registers, a device of
                `devices` comes from a plugin not in `plugins`, as `devices.<P>[i]`, a
                memory_init symbol or range is invalid, an I2C target sits on a peripheral that
                is not an I2C bus or an SPI device or controller on one that is not an SPI bus,
                an SPI controller has an address or a cs, two `devices` keys name
                one peripheral, `Machine(devices=)` refuses them, `audio` is on a peripheral
                that is not an SAI with the built-in model, or `host` on one that is not a USB
                device controller with the built-in model.
            MissingInput: An image, `svd` or `description` file, a plugin's source directory
                or library does not exist, or the ST install the description is built from.
            ImageError: An image cannot be read or does not fit the memory map.
            ModelError: A `connect` peripheral is neither a UART nor a USB device controller
                with a `host`.
        """
        check_machine_name(name)
        check_inputs(self)
        description = self.part_description()
        served = {p.name for p in description.peripherals if p.alias_of is None}
        given = _given_devices(description, devices or {})
        for key, targets in (devices or {}).items():
            for index, target in enumerate(targets):
                refused = _device_plugin_refusal(type(target), self.plugins)
                if refused is not None:
                    raise _plain_error(f"devices.{key}[{index}]", refused)
        self._check_board()
        for periph_name, periph in self.board.items():
            if periph.model is not None and periph_name not in served:
                raise _plain_error(f"board.{periph_name}", f"a peripheral of {description.part}")
        ips = {p.name: p.ip for p in description.peripherals}
        audio: dict[str, dict[Block, AudioOptions]] = {}
        for periph_name, periph in self.board.items():
            for letter, options in periph.audio.items():
                if periph.model is not None or ips.get(periph_name) != "SAI":
                    raise _plain_error(
                        f"board.{periph_name}.{letter}",
                        "an audio block of an SAI with the built-in model",
                    )
                audio.setdefault(periph_name, {})[letter] = options
        usb: dict[str, UsbHostOptions] = {}
        for periph_name, periph in self.board.items():
            if periph.host is None:
                continue
            if periph.model is not None or ips.get(periph_name) != "USB_OTG_FS":
                raise _plain_error(
                    f"board.{periph_name}.host", "a USB device controller with the built-in model"
                )
            usb[periph_name] = periph.host
        loaded = self._loaded_plugins()
        models: dict[str, Callable[[], Model]] = {}
        plugin_models: dict[str, tuple[_records.Loaded, str]] = {}
        for periph_name, periph in self.board.items():
            model = periph.model
            if isinstance(model, Use):
                models[periph_name] = functools.partial(_built_model, model)
            elif isinstance(model, PluginModel):
                record = loaded[model.plugin.name]
                if model.name not in record.models:
                    known = ", ".join(record.models) or "none"
                    raise _plain_error(
                        f"board.{periph_name}.model",
                        f"a model plugin {model.plugin.name} registers, one of {known}",
                    )
                plugin_models[periph_name] = (record, model.name)
        attached = {
            periph_name: self._devices(periph_name, periph, description)
            for periph_name, periph in self.board.items()
            if periph.devices and periph_name not in given
        }
        attached.update(given)
        boot = self.boot
        m = Machine(
            description,
            image=tuple(image.load() for image in self.images),
            core_hz=self.core_hz or 0,
            cpi=self.cpi,
            strict_peripherals=self.strict if strict_peripherals is None else strict_peripherals,
            vector_table=boot.vtor,
            config_digest=self.digest,
            arena_base=_arena_base(boot.arena_base),
            arena_size=DEFAULT_ARENA_SIZE if boot.arena_size is None else boot.arena_size,
            models=models,
            _plugin_models=plugin_models,
            devices=attached,
            rcc=self.rcc,
            audio=audio,
            usb=usb,
            supply=self.supply,
            name=name,
        )
        for periph_name, periph in self.board.items():
            if periph.connect is None:
                continue
            if periph.connect != "pty":
                raise _plain_error(f"board.{periph_name}.connect", f'"pty", not "{periph.connect}"')
            m.board.pty(periph_name)
        for index, init in enumerate(boot.memory_init):
            key = f"boot.memory_init[{index}]"
            try:
                address = m.address_of(init.symbol)
            except SymbolError as error:
                raise _plain_error(f"{key}.symbol", "a symbol of the loaded images") from error
            try:
                m.mem.write(address, init.data)
            except (ConfigError, EmulationError) as error:
                raise _plain_error(f"{key}.data", f"bytes the memory accepts: {error}") from error
        return m

    async def boot_machine(self, machine: Machine) -> Stop:
        """Reset `machine` and run it to `boot.until` within `boot.budget`.

        Returns:
            The ADDRESS stop at `until`.

        Raises:
            ConfigError: `boot.until` is not set or names no symbol.
            FaultError: The firmware faulted before reaching `until`.
            BudgetExceeded: The budget ran out before `until`.
            EmulationError: Any other stop before `until`.
        """
        boot = self.boot
        if boot.until is None:
            raise _plain_error("boot.until", "a symbol boot runs to")
        try:
            target = machine.address_of(boot.until)
        except SymbolError as error:
            raise _plain_error("boot.until", "a symbol of the loaded images") from error
        machine.reset()
        stop = await machine.run(until=target, budget=boot.budget)
        if stop.reason is StopReason.ADDRESS and stop.pc == target:
            return stop
        message = f"{error_prefix(machine)}boot did not reach {boot.until}: {stop}"
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

    def _loaded_plugins(self) -> dict[str, _records.Loaded]:
        loaded: dict[str, _records.Loaded] = {}
        for index, plugin in enumerate(self.plugins):
            key = f"plugins[{index}]"
            try:
                record = _records.load(plugin, plugin.built())
            except ConfigError as error:
                raise ConfigError(
                    f"{key}: {error}",
                    file=error.file,
                    key=key,
                    expected=error.expected,
                ) from error
            loaded[plugin.name] = record
        return loaded

    def _check_board(self) -> None:
        for name, periph in self.board.items():
            key = f"board.{name}"
            if isinstance(periph.model, Use):
                model = periph.model.cls
                if not issubclass(model, Model):
                    raise _plain_error(
                        f"{key}.model",
                        f"a popcorn.model.Model subclass, not {_reference(periph.model.cls)}",
                    )
                self._check_params(model, (), dict(periph.model.params), key)
            for index, device in enumerate(periph.devices):
                at = f"{key}.devices[{index}]"
                cls = device.cls
                if not issubclass(
                    cls, _core.I2CTargetBase | _core.SpiTargetBase | _core.SpiControllerBase
                ):
                    expected = "a popcorn.board.I2CTarget, SpiTarget or SpiController subclass"
                    raise _plain_error(f"{at}.model", f"{expected}, not {_reference(device.cls)}")
                params = dict(device.params)
                if issubclass(cls, _core.I2CTargetBase):
                    if device.address is None and device.cs is not None:
                        raise _plain_error(f"{at}.cs", "no cs for an I2C target")
                    if device.address is None:
                        raise _plain_error(f"{at}.address", "the address of an I2C target")
                    self._check_params(cls, (device.address,), params, at)
                elif issubclass(cls, _core.SpiControllerBase):
                    if device.address is not None:
                        raise _plain_error(f"{at}.address", "no address for an SPI controller")
                    if device.cs is not None:
                        raise _plain_error(f"{at}.cs", "no cs for an SPI controller")
                    self._check_params(cls, (), params, at)
                else:
                    if device.address is not None:
                        raise _plain_error(
                            f"{at}.address", "no address for an SPI device, it has a cs"
                        )
                    if device.cs is not None and not _takes_cs(cls):
                        raise _plain_error(
                            f"{at}.cs", f"a cs keyword accepted by {cls.__qualname__}"
                        )
                    chip: dict[str, Param] = {} if device.cs is None else {"cs": device.cs}
                    self._check_params(cls, (), {**chip, **params}, at)

    def _devices(
        self, name: str, periph: Periph, description: PartDescription
    ) -> list[AttachedDevice]:
        peripheral = next(
            (p for p in description.peripherals if p.name == name and p.alias_of is None), None
        )
        ip = None if peripheral is None else peripheral.ip
        served = {p.name for p in description.peripherals if p.alias_of is None}
        built: list[AttachedDevice] = []
        for index, device in enumerate(periph.devices):
            at = f"board.{name}.devices[{index}]"
            cls = device.cls
            params = _thawed_params(device)
            make = cast(Callable[..., AttachedDevice], cls)
            try:
                if issubclass(cls, _core.I2CTargetBase):
                    if ip != "I2C":
                        raise _plain_error(f"{at}.model", f"an I2C bus of {description.part}")
                    built.append(make(cast(int, device.address), **params))
                elif issubclass(cls, _core.SpiTargetBase):
                    if ip != "SPI":
                        raise _plain_error(f"{at}.model", f"an SPI bus of {description.part}")
                    if device.cs is not None and f"GPIO{device.cs[1]}" not in served:
                        raise _plain_error(
                            f"{at}.cs", f"a chip select on a GPIO port of {description.part}"
                        )
                    chip = {} if device.cs is None else {"cs": device.cs}
                    built.append(make(**chip, **params))
                elif issubclass(cls, _core.SpiControllerBase):
                    if ip != "SPI":
                        raise _plain_error(f"{at}.model", f"an SPI bus of {description.part}")
                    built.append(make(**params))
            except (ValueError, TypeError) as error:
                raise _plain_error(at, f"arguments {cls.__qualname__} accepts: {error}") from error
        return built

    def _check_params(
        self, cls: type, args: tuple[object, ...], params: dict[str, Param], key: str
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
                        raise _plain_error(
                            f"{key}.{param}",
                            f"a parameter of {cls.__qualname__}: {', '.join(named) or 'none'}",
                        ) from error
            for param in accepted[len(args) :]:
                if (
                    param.default is param.empty
                    and param.kind in (param.POSITIONAL_OR_KEYWORD, param.KEYWORD_ONLY)
                    and param.name not in params
                ):
                    raise _plain_error(
                        f"{key}.{param.name}", f"a value for {cls.__qualname__}({param.name})"
                    ) from error
            raise _plain_error(key, f"arguments {cls.__qualname__} accepts: {error}") from error


def check_inputs(description: MachineDescription) -> None:
    """Raise for the first file of `description` that does not exist: an image, `description`,
    `svd` when no `description` replaces it, a plugin's source directory or library. A file that
    exists but cannot be read is left to its reader.

    Raises:
        MissingInput: That file, with its key.
    """
    paths: list[tuple[str, Path | None]] = [
        (f"images[{i}].path", image.path) for i, image in enumerate(description.images)
    ]
    if description.description is None:
        paths.append(("svd", description.svd))
    paths.append(("description", description.description))
    for index, plugin in enumerate(description.plugins):
        if plugin.source is not None:
            paths.append((f"plugins[{index}].source", plugin.source))
        else:
            paths.append((f"plugins[{index}].library", plugin.library))
    for key, path in paths:
        if path is not None and absent(path):
            raise MissingInput(f"{key}: no file at {path}", path=path, key=key)


def _plain_error(key: str, expected: str) -> ConfigError:
    return ConfigError(f"{key}: expected {expected}", key=key, expected=expected)


def _frozen_params(params: Mapping[str, object]) -> _FrozenMap[str, Param]:
    return _FrozenMap({name: _frozen(name, value) for name, value in params.items()})


def _frozen(key: str, value: object) -> Param:
    if isinstance(value, float) and not math.isfinite(value):
        raise _plain_error(key, f"a finite number, not {value}")
    if value is None or isinstance(value, bool | int | float | str | bytes | VirtualDuration):
        return value
    if isinstance(value, Path):
        return value
    if isinstance(value, list | tuple):
        items = cast(Sequence[object], value)
        return tuple(_frozen(f"{key}[{index}]", item) for index, item in enumerate(items))
    if isinstance(value, Mapping):
        mapping = cast(Mapping[object, object], value)
        if all(isinstance(k, str) for k in mapping):
            return _FrozenMap[str, Param](
                {str(k): _frozen(f"{key}.{k}", v) for k, v in mapping.items()}
            )
        if all(isinstance(k, int) and not isinstance(k, bool) for k in mapping):
            return _FrozenMap[int, Param](
                {cast(int, k): _frozen(f"{key}.{k}", v) for k, v in mapping.items()}
            )
        raise _plain_error(key, "a mapping whose keys are all str or all int")
    raise _plain_error(
        key,
        "None, bool, int, float, str, bytes, Path, VirtualDuration, or a list, tuple or "
        f"mapping of these, not {type(value).__qualname__}",
    )


def _thawed(value: Param) -> object:
    if isinstance(value, tuple):
        return [_thawed(item) for item in value]
    if isinstance(value, _FrozenMap):
        mapping = cast(_FrozenMap[object, Param], value)
        return {k: _thawed(v) for k, v in mapping.items()}
    return value


def _thawed_params(use: Use) -> dict[str, object]:
    return {name: _thawed(value) for name, value in use.params.items()}


def _built_model(use: Use) -> Model:
    return cast(Model, use.cls(**_thawed_params(use)))


def _reference(cls: type) -> str:
    return f"{cls.__module__}:{cls.__qualname__}"


def _kind(periph: Periph) -> str:
    return "plugin" if isinstance(periph.model, PluginModel) else "Python"


def _device_plugin_refusal(cls: type, plugins: Sequence[Plugin]) -> str | None:
    """What a device of class @p cls needs and lacks, None when every class of its MRO that
    comes from a plugin's bindings module names a plugin of @p plugins."""
    prefix = f"{_records.MODULE_PREFIX}."
    for base in cls.__mro__:
        module = base.__module__
        if not module.startswith(prefix):
            continue
        plugin = _records.recorded(module)
        if plugin is None:
            return (
                f"a device class of a bindings module Plugin.module() imported, not"
                f" {_reference(cls)} from {module}"
            )
        if plugin not in plugins:
            return (
                f"a device of a plugin in plugins, {_reference(cls)} derives from"
                f" {_reference(base)} of plugin {plugin.name}"
            )
    return None


def _plugin_identity(plugin: Plugin) -> dict[str, object]:
    if plugin.source is not None:
        key = _build_cache.known_key(plugin.name, plugin.source)
        if key is None:
            key = _build_cache.key_of(plugin.source)
        return {"$": "Plugin", "name": plugin.name, "key": key}
    assert plugin.library is not None
    digest = hashlib.sha256()
    files = [plugin.library, *sorted(plugin.library.parent.glob("_native*.so"))]
    for path in files:
        if not path.is_file():
            digest.update(f"{path.name}\0absent\0".encode())
            continue
        content = path.read_bytes()
        digest.update(f"{path.name}\0{len(content)}\0".encode())
        digest.update(content)
    return {"$": "Plugin", "name": plugin.name, "sha256": digest.hexdigest()}


def _canonical(value: object) -> object:
    if value is None or isinstance(value, bool | int | float | str):
        return value
    if isinstance(value, bytes):
        return {"bytes": value.hex()}
    if isinstance(value, Path):
        return {"path": value.as_posix()}
    if isinstance(value, type):
        return {"class": _reference(value)}
    if isinstance(value, MachineDescription):
        skipped = {"name", "strict", "missing", "configs"}
        if not value.plugins:
            skipped.add("plugins")
        fields = {
            f.name: _canonical(getattr(value, f.name))
            for f in dataclasses.fields(value)
            if f.name not in skipped
        }
        return {"$": "MachineDescription", **fields, "cpi": float(value.cpi)}
    if isinstance(value, Plugin):
        return _plugin_identity(value)
    if isinstance(value, PluginModel):
        return {"$": "PluginModel", "plugin": value.plugin.name, "model": value.name}
    if dataclasses.is_dataclass(value):
        fields = {f.name: _canonical(getattr(value, f.name)) for f in dataclasses.fields(value)}
        return {"$": type(value).__qualname__, **fields}
    if isinstance(value, Mapping):
        mapping = cast(Mapping[object, object], value)
        if all(isinstance(k, str) for k in mapping):
            return {"map": {str(k): _canonical(v) for k, v in mapping.items()}}
        pairs = sorted(cast(Mapping[int, object], mapping).items())
        return {"int_map": [[k, _canonical(v)] for k, v in pairs]}
    if isinstance(value, list | tuple):
        return [_canonical(item) for item in cast(Sequence[object], value)]
    raise TypeError(f"not a machine description value: {value!r}")


def _given_devices(
    description: PartDescription, devices: Mapping[str, Sequence[AttachedDevice]]
) -> dict[str, list[AttachedDevice]]:
    aliases = {p.name: p.alias_of for p in description.peripherals if p.alias_of is not None}
    given: dict[str, list[AttachedDevice]] = {}
    for key, targets in devices.items():
        name = aliases.get(key, key)
        if name in given:
            raise ConfigError(
                f"devices.{key}: {name} is listed twice, once through an alias",
                key=f"devices.{key}",
                expected="each peripheral once",
            )
        given[name] = list(targets)
    return given


def _arena_base(value: int | Literal[False] | None) -> int | None:
    if value is None:
        return DEFAULT_ARENA_BASE
    if value is False:
        return None
    return value


def _takes_cs(cls: type) -> bool:
    try:
        parameters = inspect.signature(cls).parameters.values()
    except TypeError, ValueError:
        return True
    return any(
        p.kind is p.VAR_KEYWORD
        or (p.name == "cs" and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY))
        for p in parameters
    )
