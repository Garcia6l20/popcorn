"""JSON and folded stack outputs of a profile report."""

from __future__ import annotations

import dataclasses
import json
import types
import typing
from collections.abc import Callable, Iterator, Sequence
from enum import Enum
from typing import Any, TextIO, cast

from popcorn.profiling._tables import FrameKind, FrameRow

SCHEMA = 1
"""Version of the JSON a report writes, `Report.to_json`."""


def _decode(kind: Any, value: Any) -> Any:
    origin = typing.get_origin(kind)
    if origin in (typing.Union, types.UnionType):
        if value is None:
            return None
        inner = next(arg for arg in typing.get_args(kind) if arg is not type(None))
        return _decode(inner, value)
    if origin is tuple:
        args = typing.get_args(kind)
        if len(args) == 2 and args[1] is Ellipsis:
            return tuple(_decode(args[0], item) for item in value)
        return tuple(_decode(arg, item) for arg, item in zip(args, value, strict=True))
    if origin is dict:
        key, item = typing.get_args(kind)
        return {_decode(key, k): _decode(item, v) for k, v in value.items()}
    if dataclasses.is_dataclass(kind):
        hints = typing.get_type_hints(kind)
        build = cast(Callable[..., Any], kind)
        return build(
            **{
                field.name: _decode(hints[field.name], value[field.name])
                for field in dataclasses.fields(kind)
            }
        )
    if isinstance(kind, type) and issubclass(kind, Enum):
        return kind(value)
    if kind is float:
        return float(value)
    return value


def to_json(report: Any) -> str:
    """`report` as JSON text, `{"schema": SCHEMA, "report": {...}}`."""
    return json.dumps({"schema": SCHEMA, "report": dataclasses.asdict(report)})


def from_json[T](kind: type[T], text: str) -> T:
    """The `kind` a `to_json` text holds. Each field decodes as its annotation names, a leaf
    of another JSON type than its annotation decodes as given.

    Raises:
        ValueError: The text is no report of schema `SCHEMA`.
    """
    loaded: Any = json.loads(text)
    if not isinstance(loaded, dict):
        raise ValueError(f"no profile report of schema {SCHEMA}")
    data = cast(dict[str, Any], loaded)
    schema = data.get("schema")
    if type(schema) is not int or schema != SCHEMA or "report" not in data:
        raise ValueError(f"no profile report of schema {SCHEMA}")
    try:
        return _decode(kind, data["report"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"malformed profile report: {error}") from error


def _frame_name(kind: FrameKind, name: str) -> str:
    label = f"[{name}]" if kind is FrameKind.EXCEPTION else name
    return label.replace(";", ":").replace("\n", " ")


def folded_lines(frames: Sequence[FrameRow]) -> Iterator[str]:
    """One `root;...;leaf self` line per call tree node that executed instructions itself."""
    paths: list[str] = []
    for index, frame in enumerate(frames):
        if index == 0:
            paths.append("")
            continue
        own = _frame_name(frame.kind, frame.name)
        parent = paths[frame.parent]
        paths.append(f"{parent};{own}" if parent else own)
        if frame.self:
            yield f"{paths[index]} {frame.self}"


def write_folded(frames: Sequence[FrameRow], out: TextIO) -> int:
    """Writes the folded lines of `frames` to `out`, returns the line count."""
    count = 0
    for line in folded_lines(frames):
        out.write(line + "\n")
        count += 1
    return count
