"""The on-disk boot snapshot cache, FR-SNAP-3: a later session restores instead of booting."""

from __future__ import annotations

import asyncio
import contextlib
import fcntl
import functools
import hashlib
import inspect
import json
import os
from collections.abc import AsyncGenerator, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest

import popcorn
from popcorn import Machine, Project, Snapshot, Stop, StopReason, _core, description_digest
from popcorn._devices import ParamError, project_params
from popcorn.units import VirtualTime

FORMAT = "popcorn-boot-cache 1"


@functools.cache
def _extension_digest() -> str:
    path = getattr(_core, "__file__", None)
    if not isinstance(path, str):
        return ""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


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


def _param_files(cls: type, params: Mapping[str, object], base: Path) -> list[str]:
    try:
        resolved = project_params(cls, params, base)
    except ParamError:
        return []
    files: list[str] = []
    for name, value in sorted(resolved.items()):
        if isinstance(value, Path):
            digest = _file_digest(value) if value.is_file() else "missing"
            files.append(f"{name} {value} {digest}")
    return files


def cache_key(project: Project, strict: bool) -> str:
    """The name of a boot of `project`, as its config applies, with strictness `strict`.

    It covers the popcorn version and native extension, the description, every image, the
    project file with its config, `strict`, every `.py` file under the `[models] python`
    directories, the source file of every board model and device class, and the content of every
    file a model or device parameter names. A module outside
    those directories that a model imports is not covered: `--cache-clear` drops the cache.
    """
    lines = [
        FORMAT,
        popcorn.version(),
        _extension_digest(),
        description_digest(project.description()),
        *(image.digest for image in project.images()),
        project.digest,
        str(strict),
    ]
    for directory in project.file.models.python:
        root = Path(directory)
        for path in sorted(root.rglob("*.py")):
            lines.append(f"{path.relative_to(root)} {_file_digest(path)}")
    base = project.path.parent
    for name, entry in sorted(project.board().items()):
        parts = [(entry.model, entry.params)] if entry.model is not None else []
        parts += [(device.model, device.params) for device in entry.devices]
        for cls, params in parts:
            lines.append(f"{name} {_class_source(cls)}")
            lines.extend(f"{name} {file}" for file in _param_files(cls, params, base))
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
