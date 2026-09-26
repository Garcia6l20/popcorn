"""Coverage sets in the .pcov format: load, save, merge and export."""

from __future__ import annotations

import os
import re
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Final

from popcorn import _obs

_WORKER_ID: Final = re.compile(r"[A-Za-z0-9_-]+")

type PathLike = str | os.PathLike[str]


class CoverageFileError(ValueError):
    """A .pcov file is malformed, or an ELF it names changed since the coverage was collected."""


class ImageKind(IntEnum):
    """What a loaded image was loaded from."""

    ELF = 0
    RAW = 1


@dataclass(frozen=True, slots=True)
class ImageRef:
    """An image a coverage group was collected against."""

    kind: ImageKind
    path: str
    digest: str
    base: int
    end: int

    @classmethod
    def from_elf(cls, path: PathLike) -> ImageRef:
        """Reference of an ELF file: absolute path, digest and executable extent."""
        kind, absolute, digest, base, end = _call(_obs.image_ref_of, os.fspath(path))
        return cls(ImageKind(kind), absolute, digest, base, end)


@dataclass(frozen=True, slots=True)
class CodeRange:
    """A code range coverage was enabled over."""

    name: str
    start: int
    size: int


@dataclass(frozen=True, slots=True)
class BlockHit:
    """Executions of one translated block of a given byte size."""

    start: int
    size: int
    hits: int


@dataclass(frozen=True, slots=True)
class EdgeHit:
    """Executions of one edge between block starts."""

    source: int
    target: int
    hits: int


@dataclass(frozen=True, slots=True)
class CoverageGroup:
    """Coverage collected against one list of images."""

    images: tuple[ImageRef, ...]
    ranges: tuple[CodeRange, ...]
    blocks: tuple[BlockHit, ...]
    edges: tuple[EdgeHit, ...]
    split: int = 0
    dropped: int = 0


def _call[**P, R](function: Callable[P, R], *args: P.args, **kwargs: P.kwargs) -> R:
    try:
        return function(*args, **kwargs)
    except _obs.FormatError as error:
        raise CoverageFileError(str(error)) from error


class Coverage:
    """Coverage of one or more runs, held as canonical .pcov bytes. Equal when the bytes are."""

    __slots__ = ("_data", "_groups")

    def __init__(self, data: bytes) -> None:
        """Wraps .pcov bytes, checked and made canonical.

        Raises:
            CoverageFileError: the bytes are not a .pcov file.
        """
        self._data: bytes = _call(_obs.merge_pcov, [data])
        self._groups: tuple[CoverageGroup, ...] | None = None

    @classmethod
    def load(cls, path: PathLike) -> Coverage:
        """Reads a .pcov file.

        Raises:
            FileNotFoundError: the file does not exist.
            CoverageFileError: the file is not a .pcov file, the message names it.
        """
        data = Path(path).read_bytes()
        try:
            return cls(data)
        except CoverageFileError as error:
            raise CoverageFileError(f"{os.fspath(path)}: {error}") from error

    @classmethod
    def from_parts(cls, groups: Sequence[CoverageGroup]) -> Coverage:
        """Builds coverage from its groups."""
        rows = [
            (
                [(int(i.kind), i.path, i.digest, i.base, i.end) for i in group.images],
                [(r.name, r.start, r.size) for r in group.ranges],
                [(b.start, b.size, b.hits) for b in group.blocks],
                [(e.source, e.target, e.hits) for e in group.edges],
                group.split,
                group.dropped,
            )
            for group in groups
        ]
        return cls(_call(_obs.encode_pcov, rows))

    @property
    def groups(self) -> tuple[CoverageGroup, ...]:
        """Groups, sorted by their image digests."""
        if self._groups is None:
            self._groups = tuple(
                CoverageGroup(
                    images=tuple(
                        ImageRef(ImageKind(kind), path, digest, base, end)
                        for kind, path, digest, base, end in images
                    ),
                    ranges=tuple(CodeRange(*r) for r in ranges),
                    blocks=tuple(BlockHit(*b) for b in blocks),
                    edges=tuple(EdgeHit(*e) for e in edges),
                    split=split,
                    dropped=dropped,
                )
                for images, ranges, blocks, edges, split, dropped in _call(
                    _obs.describe_pcov, self._data
                )
            )
        return self._groups

    def to_bytes(self) -> bytes:
        """The canonical .pcov bytes."""
        return self._data

    def save(self, path: PathLike) -> None:
        """Writes a .pcov file, through a unique temporary file in the same directory renamed over
        the destination, so that a reader never sees half a file."""
        import tempfile

        destination = Path(path)
        handle, name = tempfile.mkstemp(dir=destination.parent, prefix=f"{destination.name}.tmp.")
        temporary = Path(name)
        try:
            with os.fdopen(handle, "wb") as out:
                out.write(self._data)
            temporary.replace(destination)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise

    def merge_into(self, path: PathLike) -> Coverage:
        """Adds this coverage to a .pcov file, created when missing, and returns the sum.

        What the machines of one worker do with the worker's file. One writer per file: two
        processes adding to the same file at once can lose one's coverage.

        Raises:
            CoverageFileError: the existing file is not a .pcov file.
        """
        destination = Path(path)
        merged = merge([destination, self]) if destination.exists() else self
        merged.save(destination)
        return merged

    def export_lcov(self, path: PathLike, *, branches: bool = True, test_name: str = "") -> None:
        """Writes an lcov tracefile, the ELF images loaded from their recorded paths.

        Raises:
            CoverageFileError: an ELF changed since the coverage was collected.
        """
        Path(path).write_text(_call(_obs.lcov_text, self._data, branches, test_name))

    def export_drcov(self, path: PathLike) -> int:
        """Writes a drcov version 2 file for Lighthouse and other drcov readers.

        One module per image. drcov has no counts: every block that ran is listed once per
        module.

        Returns:
            How many blocks were left out, outside every image's executable extent.
        """
        data, dropped = _call(_obs.drcov_bytes, self._data)
        Path(path).write_bytes(data)
        return dropped

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Coverage) and self._data == other._data

    def __hash__(self) -> int:
        return hash(self._data)


def merge(sources: Iterable[PathLike | Coverage]) -> Coverage:
    """Sums coverage from .pcov files and Coverage objects. The order does not matter.

    Raises:
        ValueError: no source was given.
        FileNotFoundError: a file does not exist.
        CoverageFileError: a file is not a .pcov file, the message names it.
    """
    coverages = [
        source if isinstance(source, Coverage) else Coverage.load(source) for source in sources
    ]
    if not coverages:
        raise ValueError("merge needs at least one source")
    return Coverage(_call(_obs.merge_pcov, [coverage.to_bytes() for coverage in coverages]))


def worker_path(directory: PathLike, worker: str) -> Path:
    """The .pcov file of one pytest-xdist worker in a session directory.

    Raises:
        ValueError: the worker id is not made of letters, digits, underscores and dashes.
    """
    if _WORKER_ID.fullmatch(worker) is None:
        raise ValueError(f"worker id {worker!r} is not made of [A-Za-z0-9_-]")
    return Path(directory) / f"{worker}.pcov"


def merge_session(directory: PathLike, lcov: PathLike, *, branches: bool = True) -> Coverage:
    """Merges every .pcov file of a session directory, writes one lcov file, returns the sum.

    Writes nothing into the directory, so calling it again gives the same result. Temporary
    files of a save in progress, `<name>.pcov.tmp.<suffix>`, are not read.

    Raises:
        ValueError: the directory holds no .pcov file.
    """
    merged = merge(sorted(Path(directory).glob("*.pcov")))
    merged.export_lcov(lcov, branches=branches)
    return merged
