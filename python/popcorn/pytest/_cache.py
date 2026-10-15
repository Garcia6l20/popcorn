"""The on-disk boot snapshot cache, FR-SNAP-3: a later session restores instead of booting."""

from __future__ import annotations

import ast
import asyncio
import contextlib
import enum
import fcntl
import functools
import hashlib
import importlib.util
import inspect
import json
import os
import sys
import types
from collections.abc import AsyncGenerator, Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest

import popcorn
from popcorn import (
    Machine,
    MachineDescription,
    PartDescription,
    Snapshot,
    Stop,
    StopReason,
    Use,
    _core,
    description_digest,
)
from popcorn._machine import AttachedDevice
from popcorn._machine_description import check_inputs
from popcorn._model_build import registered_classes
from popcorn._native_digest import native_digest
from popcorn.board import I2CTarget, SpiController, SpiTarget
from popcorn.units import VirtualTime

FORMAT = "popcorn-boot-cache 1"


@functools.cache
def _extension_digest() -> str:
    return native_digest()


def _file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _class_source(cls: type) -> str:
    try:
        source = inspect.getsourcefile(cls)
    except TypeError:
        source = None
    if source is None:
        return f"{cls.__module__}.{cls.__qualname__}"
    return f"{source} {_file_digest(Path(source))}"


def _prefixes(name: str) -> list[str]:
    parts = name.split(".")
    return [".".join(parts[: index + 1]) for index in range(len(parts))]


def _imported_names(tree: ast.Module, package: str | None) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                names.update(_prefixes(alias.name))
        elif isinstance(node, ast.ImportFrom):
            written = "." * node.level + (node.module or "")
            try:
                base = importlib.util.resolve_name(written, package) if node.level else written
            except ImportError, ValueError:
                continue
            names.update(_prefixes(base))
            names.update(f"{base}.{alias.name}" for alias in node.names if alias.name != "*")
    return names


def _versioned(path: Path) -> bool:
    roots = {Path(sys.base_prefix), Path(sys.prefix), Path(popcorn.__file__).resolve().parent}
    return "site-packages" in path.parts or any(path.is_relative_to(root) for root in roots)


def _origin(name: str) -> Path | None:
    try:
        spec = importlib.util.find_spec(name)
    except Exception:
        return None
    if spec is None or spec.origin is None:
        return None
    origin = Path(spec.origin)
    return origin if origin.is_file() else None


def imported_sources(cls: type) -> list[str]:
    """The cache key lines of the modules the source file of `cls` imports, at module level or
    inside a function, with the packages on the way to each, one level deep. Modules of
    popcorn, of the Python installation and of site-packages are left out, as versions cover
    them."""
    try:
        source = inspect.getsourcefile(cls)
    except TypeError:
        return []
    if source is None or not Path(source).is_file():
        return []
    try:
        tree = ast.parse(Path(source).read_bytes(), source)
    except SyntaxError:
        return []
    module = sys.modules.get(cls.__module__)
    package = getattr(module, "__package__", None) or cls.__module__.rpartition(".")[0]
    origins = {_origin(name) for name in _imported_names(tree, package)}
    return sorted(
        f"imports {origin} {_file_digest(origin)}"
        for origin in origins
        if origin is not None and origin != Path(source) and not _versioned(origin.resolve())
    )


def _param_files(params: Mapping[str, object]) -> list[str]:
    files: list[str] = []
    for name, value in sorted(params.items()):
        if isinstance(value, Path):
            digest = _file_digest(value) if value.is_file() else "missing"
            files.append(f"{name} {value} {digest}")
    return files


def _plain(value: object, owner: object) -> str | None:
    if value is None or isinstance(value, bool | int | float | str | bytes | bytearray):
        return repr(value)
    if isinstance(value, enum.Enum):
        return f"{type(value).__qualname__}.{value.name}"
    if isinstance(value, types.MethodType) and value.__self__ is owner:
        return f"method {value.__func__.__qualname__}"
    if isinstance(value, list | tuple | set | frozenset):
        kind = type(cast(object, value))
        items: list[str] = []
        for item in cast(Iterable[object], value):
            plain = _plain(item, owner)
            if plain is None:
                return None
            items.append(plain)
        if issubclass(kind, set | frozenset):
            items.sort()
        return f"{kind.__name__}[{', '.join(items)}]"
    if isinstance(value, dict):
        pairs: list[str] = []
        for key, item in cast(dict[object, object], value).items():
            plain_key, plain_item = _plain(key, owner), _plain(item, owner)
            if plain_key is None or plain_item is None:
                return None
            pairs.append(f"{plain_key}: {plain_item}")
        return f"dict[{', '.join(sorted(pairs))}]"
    return None


def _device_line(device: AttachedDevice) -> str | None:
    if not isinstance(device, I2CTarget | SpiTarget | SpiController):
        return None
    cls = type(device)
    if isinstance(device, SpiController):
        return f"{cls.__module__}.{cls.__qualname__} {_class_source(cls)}"
    try:
        state = device.snapshot()
        attributes = _plain(vars(device), device)
    except Exception:
        return None
    if attributes is None:
        return None
    return f"{cls.__module__}.{cls.__qualname__} {_class_source(cls)} {state.hex()} {attributes}"


def devices_digest(
    project: MachineDescription, devices: Mapping[str, Sequence[AttachedDevice]]
) -> str | None:
    """The digest of `devices` as `MachineDescription.machine(devices=)` takes them, None when a
    device cannot be keyed.

    A device is keyed by its class and the class's source file, its `snapshot()` and its
    instance attributes. It can be keyed when it is a `popcorn.board.I2CTarget` or `SpiTarget`
    whose attributes hold only None, bool, int, float, str, bytes, bytearray, enum members,
    methods of the device itself, and lists, tuples, sets and dicts of these. A
    `popcorn.board.SpiController` is keyed by its class and source only: it acts only through
    `transfer`, which needs the booted machine, so it cannot act before `boot.until`. A
    `popcorn.devices` device keeps its parameters natively and cannot be keyed.

    Not keyed: class attributes changed at run time, module globals or class-level containers
    the device reads, and the modules its class imports. A stale entry matters only when the
    firmware talks to the device before `boot.until`.

    Raises:
        MissingInput: An input file of `project` does not exist, as `MachineDescription.machine`
        raises.
    """
    check_inputs(project)
    aliases = {
        p.name: p.alias_of for p in project.part_description().peripherals if p.alias_of is not None
    }
    lines: list[str] = []
    for key, targets in sorted(devices.items(), key=lambda item: aliases.get(item[0], item[0])):
        name = aliases.get(key, key)
        lines.append(name)
        for target in targets:
            line = _device_line(target)
            if line is None:
                return None
            lines.append(f"{name} {line}")
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()[:16]


def _registration_lines(project: MachineDescription, description: PartDescription) -> list[str]:
    wired = {name for name, periph in project.board.items() if periph.model is not None}
    lines: list[str] = []
    for p in sorted(description.peripherals, key=lambda p: p.name):
        if p.alias_of is not None or p.name in wired:
            continue
        glob = _core.registered_model_glob(p.ip, p.ip_version)
        if glob is None:
            continue
        cls = registered_classes.get((p.ip, glob))
        if cls is None:
            lines.append(f"{p.name} registered {p.ip} {glob} built-in")
            continue
        head = f"{p.name} registered {p.ip} {glob} {cls.__module__}:{cls.__qualname__}"
        lines.append(f"{head} {_class_source(cls)}")
        lines.extend(f"{p.name} registered {line}" for line in imported_sources(cls))
    return lines


def cache_key(project: MachineDescription, strict: bool, devices: str | None = None) -> str:
    """The name of a boot of `project` with strictness `strict` and the device set of digest
    `devices`, None for the machine description's.

    It covers the popcorn version and native extension, the description, every image,
    `project.digest`, `strict`, the source file of every board model and device class and of
    every module its source file imports, at module level or inside a function, with the
    packages on the way to it, the content of every file a model or device parameter names,
    and `devices`. For each served peripheral without a board model, it covers the IP and glob
    of the registration that serves it as the native registry resolves it, built-in or
    `popcorn.model.register()`, and for the latter the class and its sources as for a board
    model. A registration that serves no such peripheral is left out. Modules of popcorn, of
    the Python installation and of site-packages are left out: the popcorn version, the
    extension digest and installed versions cover them. A module imported only by those
    modules is not covered: `--cache-clear` drops the cache. A plugin's code, of a
    `PluginModel` on the board or of a device class from its bindings module, is covered by
    `project.digest` through `plugins`: a dev plugin by its cache key over its sources, an
    installed one by the SHA-256 of its library and bindings module.

    Raises:
        MissingInput: An input file of `project` does not exist, as `MachineDescription.machine`
        raises.
    """
    check_inputs(project)
    description = project.part_description()
    lines = [
        FORMAT,
        popcorn.version(),
        _extension_digest(),
        description_digest(description),
        *(image.load().digest for image in project.images),
        project.digest,
        str(strict),
    ]
    if devices is not None:
        lines.append(f"devices {devices}")
    for name, periph in sorted(project.board.items()):
        uses: list[Use] = [periph.model] if isinstance(periph.model, Use) else []
        for use in [*uses, *periph.devices]:
            cls, params = use.cls, use.params
            lines.append(f"{name} {_class_source(cls)}")
            lines.extend(f"{name} {line}" for line in imported_sources(cls))
            lines.extend(f"{name} {file}" for file in _param_files(params))
    lines.extend(_registration_lines(project, description))
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()


@dataclass(frozen=True, slots=True)
class Entry:
    """The files of one cached boot.

    Attributes:
        snapshot: The saved boot snapshot, `<key>.snap`.
        info: The boot stop, `<key>.json`.
        coverage: The boot coverage, `<key>.pcov`, when the boot counted it.
        lock: The file whose lock serializes the boot of this key across processes.
    """

    snapshot: Path
    info: Path
    coverage: Path
    lock: Path

    @classmethod
    def of(cls, directory: Path, key: str) -> Entry:
        """The entry named `key` in `directory`."""
        return cls(
            directory / f"{key}.snap",
            directory / f"{key}.json",
            directory / f"{key}.pcov",
            directory / f"{key}.lock",
        )

    def exists(self, coverage: bool) -> bool:
        """Whether the entry is complete, its coverage file included when `coverage`."""
        return (
            self.snapshot.is_file()
            and self.info.is_file()
            and (not coverage or self.coverage.is_file())
        )

    def drop(self) -> None:
        """Delete the entry's files, its lock kept."""
        for path in (self.snapshot, self.info, self.coverage):
            path.unlink(missing_ok=True)

    def stop(self, machine: Machine) -> Stop | None:
        """The boot stop saved in `info`, located in `machine`, None for a boot to reset."""
        saved: object = json.loads(self.info.read_text())["stop"]
        if not isinstance(saved, dict):
            return None
        fields = cast(dict[str, object], saved)
        pc = int(cast(int, fields["pc"]))
        return Stop(
            reason=StopReason[str(fields["reason"])],
            pc=pc,
            location=machine.locate(pc),
            instructions=int(cast(int, fields["instructions"])),
            time=VirtualTime(int(cast(int, fields["time_ps"]))),
            detail=str(fields.get("detail", "")),
        )

    def save(self, snapshot: Snapshot, stop: Stop | None, coverage: bytes | None) -> None:
        """Write the entry, the snapshot last so that a complete entry has all its files."""
        self.drop()
        if coverage is not None:
            _write_atomic(self.coverage, coverage)
        info = (
            None
            if stop is None
            else {
                "reason": stop.reason.name,
                "pc": stop.pc,
                "instructions": stop.instructions,
                "time_ps": stop.time.ps,
                "detail": stop.detail,
            }
        )
        _write_atomic(self.info, json.dumps({"stop": info}).encode())
        snapshot.save(self.snapshot)


def _write_atomic(path: Path, data: bytes) -> None:
    part = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    part.write_bytes(data)
    part.replace(path)


@contextlib.asynccontextmanager
async def locked(entry: Entry) -> AsyncGenerator[None]:
    """Hold the entry's lock, waited for in a thread so the event loop keeps running.

    One process never takes it twice for one key: the session's per-key `asyncio.Lock` orders
    its boots first. `flock` is unreliable on NFS: a cache directory there may boot a key twice.
    """
    descriptor = os.open(entry.lock, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        await asyncio.to_thread(fcntl.flock, descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)


def cache_directory(config: pytest.Config) -> Path | None:
    """`.pytest_cache/d/popcorn/boot`, None without the cache plugin."""
    cache: object = getattr(config, "cache", None)
    if isinstance(cache, pytest.Cache):
        return cache.mkdir("popcorn") / "boot"
    return None
