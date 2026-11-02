"""Memo of `build_description_with_report` and `_svd.expand_svd` for the test session.

A build is keyed by the part name and the path and bytes digest of every file its `Sources` names
and of the package patches, so a test that edits a source gets its own build. The value is a zlib
level 1 pickle and the warnings the build logged, replayed to the `popcorn` logger on a hit, so a
caller always owns its result and still sees the build's warnings. The memo stands down for a call
while any module of the description package, `Path.read_bytes` or `Path.read_text` is monkeypatched,
or while the `popcorn` logger drops warnings.

`expand_svd` is memoised by the same machinery, keyed by the digest of the SVD bytes and the `file`
argument. Every shipped part of a family reads one SVD, so the expansion runs once per SVD and not
once per part, and each build still runs its own patches, rzone and compare.

`BuildMemo.share` adds a directory shared by the workers of one run. A worker missing in memory
takes the file lock of the key, loads the entry another worker wrote or builds and writes it, so
a part is built once per run whatever the worker split.
"""

import fcntl
import hashlib
import logging
import os
import pickle
import sys
import threading
import zlib
from collections import OrderedDict
from collections.abc import Callable, Generator, Mapping
from contextlib import contextmanager
from dataclasses import fields
from functools import wraps
from pathlib import Path
from typing import Any, cast

from popcorn.stm32.description import _builder, _sources
from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._patches import PatchReport
from popcorn.stm32.description._sources import Sources
from popcorn.stm32.description._svd import SvdDevice

_PACKAGE = "popcorn.stm32.description"
_LOGGER = logging.getLogger("popcorn")
_MAX_BYTES = 256 * 1024 * 1024
_MISSING = object()

_Build = Callable[[Sources], tuple[PartDescription, PatchReport]]
_Expand = Callable[..., SvdDevice]
_Entry = tuple[bytes, list[logging.LogRecord]]


class _Capture(logging.Handler):
    def __init__(self) -> None:
        super().__init__()
        self.records: list[logging.LogRecord] = []

        self._thread = threading.get_ident()

    def emit(self, record: logging.LogRecord) -> None:
        if record.thread == self._thread:
            self.records.append(record)


class BuildMemo:
    def __init__(self, build: _Build, expand: _Expand | None = None) -> None:
        self._build = build
        self._expand = expand
        self.entries: OrderedDict[tuple[object, ...], _Entry] = OrderedDict()
        self._size = 0
        self._pristine: dict[str, dict[str, object]] = {}
        self._read_bytes = Path.read_bytes
        self._read_text = Path.read_text
        self.shared: Path | None = None
        self._bypassing = 0
        self.hits = 0
        self.misses = 0
        self.loads = 0
        self.bypasses = 0

    def _modules(self) -> list[tuple[str, dict[str, object]]]:
        return [
            (name, vars(module))
            for name, module in list(sys.modules.items())
            if name == _PACKAGE or name.startswith(_PACKAGE + ".")
        ]

    def snapshot(self, *, refresh: bool = False) -> None:
        """Record the current attributes of the description modules as unpatched. Modules seen
        before keep their record unless @p refresh, for a wrapper installed on purpose."""
        for name, namespace in self._modules():
            if refresh or name not in self._pristine:
                self._pristine[name] = dict(namespace)

    def _patched(self) -> bool:
        if Path.read_bytes is not self._read_bytes or Path.read_text is not self._read_text:
            return True
        self.snapshot()
        for name, namespace in self._modules():
            pristine = self._pristine[name]
            if any(namespace.get(key, _MISSING) is not value for key, value in pristine.items()):
                return True
        return False

    def _value_key(self, value: object) -> object:
        if value is None or isinstance(value, str | int | bool):
            return value
        if isinstance(value, Path):
            return str(value), hashlib.blake2b(value.read_bytes(), digest_size=16).digest()
        if isinstance(value, Mapping):
            items = cast(Mapping[object, object], value)
            return tuple(
                (self._value_key(name), self._value_key(items[name]))
                for name in sorted(items, key=str)
            )
        if isinstance(value, list | tuple):
            return tuple(self._value_key(item) for item in cast(list[object], value))
        raise TypeError(type(value).__name__)

    def _key(self, sources: Sources) -> tuple[object, ...] | None:
        try:
            return (
                tuple(
                    (field.name, self._value_key(getattr(sources, field.name)))
                    for field in fields(sources)
                ),
                tuple(
                    self._value_key(path)
                    for path in _sources._patch_files()  # pyright: ignore[reportPrivateUsage]
                ),
            )
        except OSError, TypeError:
            return None

    @property
    def bypassing(self) -> bool:
        """True inside @ref bypassed."""
        return self._bypassing > 0

    @contextmanager
    def bypassed(self) -> Generator[None]:
        """Build every call in the block through the builder itself, nothing read from or kept in
        the memo."""
        self._bypassing += 1
        try:
            yield
        finally:
            self._bypassing -= 1

    def share(self, directory: Path) -> None:
        """Keep every entry in @p directory too, so all the workers using it share the builds."""
        directory.mkdir(parents=True, exist_ok=True)
        self.shared = directory

    def _replay(self, records: list[logging.LogRecord]) -> None:
        for record in records:
            if _LOGGER.isEnabledFor(record.levelno):
                _LOGGER.handle(record)

    def _remember(self, key: tuple[object, ...], entry: _Entry) -> None:
        self.entries[key] = entry
        self._size += len(entry[0])
        while self._size > _MAX_BYTES and len(self.entries) > 1:
            _, (dropped, _) = self.entries.popitem(last=False)
            self._size -= len(dropped)

    def _capture(self, build: Callable[[], object]) -> _Entry:
        capture = _Capture()
        _LOGGER.addHandler(capture)
        try:
            result = build()
        finally:
            _LOGGER.removeHandler(capture)
        return (
            zlib.compress(pickle.dumps(result, protocol=pickle.HIGHEST_PROTOCOL), 1),
            capture.records,
        )

    def _shared_entry(self, key: tuple[object, ...], build: Callable[[], object]) -> _Entry:
        assert self.shared is not None
        stem = self.shared / hashlib.sha256(repr(key).encode()).hexdigest()
        with open(stem.with_suffix(".lock"), "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            entry_path = stem.with_suffix(".pkl")
            if entry_path.exists():
                self.loads += 1
                loaded: _Entry = pickle.loads(entry_path.read_bytes())
                self._replay(loaded[1])
                return loaded
            entry = self._capture(build)
            partial = stem.with_suffix(f".{os.getpid()}.tmp")
            partial.write_bytes(pickle.dumps(entry, protocol=pickle.HIGHEST_PROTOCOL))
            partial.replace(entry_path)
            return entry

    def _cached(self, key: tuple[object, ...] | None, build: Callable[[], Any]) -> Any:
        if key is None or not _LOGGER.isEnabledFor(logging.WARNING):
            self.bypasses += 1
            return build()
        entry = self.entries.get(key)
        if entry is not None:
            self.entries.move_to_end(key)
            self.hits += 1
            self._replay(entry[1])
            return pickle.loads(zlib.decompress(entry[0]))
        self.misses += 1
        entry = self._shared_entry(key, build) if self.shared else self._capture(build)
        self._remember(key, entry)
        return pickle.loads(zlib.decompress(entry[0]))

    def __call__(self, sources: Sources) -> tuple[PartDescription, PatchReport]:
        key = None if self._bypassing or self._patched() else self._key(sources)
        return self._cached(key, lambda: self._build(sources))

    def expand_svd(self, data: bytes, *, file: str) -> SvdDevice:
        """`_svd.expand_svd` of @p data and @p file, run once per distinct pair. Each caller
        receives its own copy. A call that raises keeps nothing."""
        assert self._expand is not None
        expand = self._expand
        key = (
            None
            if self._bypassing or self._patched()
            else ("svd", hashlib.blake2b(data, digest_size=16).digest(), file)
        )
        return self._cached(key, lambda: expand(data, file=file))


def install() -> BuildMemo:
    """Replace `_builder.build_description_with_report` and `_builder.expand_svd` by memos of
    them, once per process.
    @return The memo, its `hits`, `misses` and `bypasses` count the calls of both."""
    current: Any = _builder.build_description_with_report
    if isinstance(current, BuildMemo):
        return current
    expand: Any = _builder.expand_svd  # pyright: ignore[reportPrivateImportUsage]
    memo = BuildMemo(current, expand)
    wraps(current)(memo)
    _builder.build_description_with_report = memo

    @wraps(expand)
    def expand_svd(data: bytes, *, file: str) -> SvdDevice:
        return memo.expand_svd(data, file=file)

    _builder.expand_svd = expand_svd  # pyright: ignore[reportPrivateImportUsage]
    memo.snapshot()
    return memo
