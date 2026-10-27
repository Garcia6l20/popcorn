"""Generic binder between the description dataclasses and JSON-ready mappings.

No per-type serializer: every dataclass and enum is walked through
``dataclasses.fields`` and ``typing.get_type_hints``.
"""

import dataclasses
import json
import types
from collections.abc import Mapping
from enum import StrEnum
from typing import TYPE_CHECKING, cast, get_args, get_origin, get_type_hints

from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import PartDescription

if TYPE_CHECKING:
    from _typeshed import DataclassInstance


def _is_optional(type_hint: object) -> bool:
    return get_origin(type_hint) is types.UnionType and type(None) in get_args(type_hint)


def _strip_optional(type_hint: object) -> object:
    args = [arg for arg in get_args(type_hint) if arg is not type(None)]
    assert len(args) == 1
    return args[0]


def to_mapping(value: object) -> object:
    """JSON-ready structure: dataclass -> dict in field order, StrEnum -> value, None fields
    omitted."""
    if isinstance(value, StrEnum):
        return value.value
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        result: dict[str, object] = {}
        for f in dataclasses.fields(value):
            attr = getattr(value, f.name)
            if attr is None:
                continue
            result[f.name] = to_mapping(attr)
        return result
    if isinstance(value, list):
        return [to_mapping(item) for item in cast(list[object], value)]
    if isinstance(value, dict):
        return {key: to_mapping(item) for key, item in cast(dict[str, object], value).items()}
    return value


def _bind_value(type_hint: object, raw: object, *, file: str, key: str) -> object:
    if type_hint is bool:
        if not isinstance(raw, bool):
            raise DescriptionError(file, 0, key, "boolean")
        return raw
    if type_hint is int:
        if isinstance(raw, bool) or not isinstance(raw, int):
            raise DescriptionError(file, 0, key, "integer")
        return raw
    if type_hint is str:
        if not isinstance(raw, str):
            raise DescriptionError(file, 0, key, "string")
        return raw
    origin = get_origin(type_hint)
    if origin is list:
        if not isinstance(raw, list):
            raise DescriptionError(file, 0, key, "array")
        raw_list = cast(list[object], raw)
        (item_type,) = get_args(type_hint)
        return [
            _bind_value(item_type, item, file=file, key=f"{key}[{i}]")
            for i, item in enumerate(raw_list)
        ]
    if origin is dict:
        if not isinstance(raw, Mapping):
            raise DescriptionError(file, 0, key, "object")
        raw_mapping = cast(Mapping[str, object], raw)
        _, value_type = get_args(type_hint)
        return {
            str(item_key): _bind_value(value_type, item, file=file, key=f"{key}.{item_key}")
            for item_key, item in raw_mapping.items()
        }
    if isinstance(type_hint, type) and issubclass(type_hint, StrEnum):
        if not isinstance(raw, str):
            raise DescriptionError(file, 0, key, "string")
        try:
            return type_hint(raw)
        except ValueError:
            valid = ", ".join(member.value for member in type_hint)
            raise DescriptionError(file, 0, key, f"one of {valid}") from None
    if isinstance(type_hint, type) and dataclasses.is_dataclass(type_hint):
        return _bind_object(type_hint, raw, file=file, key=key)
    raise TypeError(f"unsupported type hint: {type_hint!r}")


def _check_selector(cls: type, instance: object, *, file: str, key: str) -> None:
    groups: dict[str, list[str]] = {}
    for f in dataclasses.fields(cls):
        group = f.metadata.get("selector_group")
        if group is not None:
            groups.setdefault(group, []).append(f.name)
    for names in groups.values():
        set_names = [name for name in names if getattr(instance, name) is not None]
        if len(set_names) != 1:
            raise DescriptionError(
                file, 0, key or cls.__name__, f"exactly one of {', '.join(names)}"
            )


def _bind_object[T: "DataclassInstance"](cls: type[T], data: object, *, file: str, key: str) -> T:
    if not isinstance(data, Mapping):
        raise DescriptionError(file, 0, key or cls.__name__, "object")
    data_mapping = cast(Mapping[str, object], data)
    hints = get_type_hints(cls)
    flds = dataclasses.fields(cls)
    field_names = {f.name for f in flds}
    kwargs: dict[str, object] = {}
    for f in flds:
        field_key = f"{key}.{f.name}" if key else f.name
        type_hint = hints[f.name]
        optional = _is_optional(type_hint)
        if f.name not in data_mapping:
            if optional:
                continue
            raise DescriptionError(file, 0, field_key, "required")
        bind_type = _strip_optional(type_hint) if optional else type_hint
        kwargs[f.name] = _bind_value(bind_type, data_mapping[f.name], file=file, key=field_key)
    unknown = sorted(set(data_mapping) - field_names)
    if unknown:
        bad_key = f"{key}.{unknown[0]}" if key else str(unknown[0])
        raise DescriptionError(file, 0, bad_key, "known key")
    instance = cls(**kwargs)
    _check_selector(cls, instance, file=file, key=key)
    return instance


def from_mapping[T: "DataclassInstance"](
    cls: type[T], data: object, *, file: str, key: str = ""
) -> T:
    """Typed value of @p cls from parsed JSON/TOML, DescriptionError on unknown key, missing
    required key, wrong type, integer out of the metadata bounds or string shorter than its
    min_length, key path in the error."""
    instance = _bind_object(cls, data, file=file, key=key)
    _check_bounds(instance, file=file, key=key)
    return instance


def check_bounds(value: object, *, file: str) -> None:
    """Raises DescriptionError when an integer field of @p value, or of a dataclass, list or
    dict nested in it, falls outside its schema minimum/maximum/const metadata, or a string
    field is shorter than its min_length metadata."""
    _check_bounds(value, file=file, key="")


def _check_bounds(value: object, *, file: str, key: str) -> None:
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        for f in dataclasses.fields(value):
            field_key = f"{key}.{f.name}" if key else f.name
            attr = getattr(value, f.name)
            if attr is None:
                continue
            if isinstance(attr, int) and not isinstance(attr, bool):
                minimum = f.metadata.get("minimum")
                maximum = f.metadata.get("maximum")
                const = f.metadata.get("const")
                if minimum is not None and attr < minimum:
                    raise DescriptionError(file, 0, field_key, f">= {minimum}")
                if maximum is not None and attr > maximum:
                    raise DescriptionError(file, 0, field_key, f"<= {maximum}")
                if const is not None and attr != const:
                    raise DescriptionError(file, 0, field_key, f"== {const}")
            if isinstance(attr, str):
                min_length = f.metadata.get("min_length")
                if min_length is not None and len(attr) < min_length:
                    raise DescriptionError(
                        file, 0, field_key, f"a string of >= {min_length} characters"
                    )
            _check_bounds(attr, file=file, key=field_key)
    elif isinstance(value, list):
        for i, item in enumerate(cast(list[object], value)):
            _check_bounds(item, file=file, key=f"{key}[{i}]")
    elif isinstance(value, dict):
        for item_key, item in cast(dict[str, object], value).items():
            _check_bounds(item, file=file, key=f"{key}.{item_key}" if key else str(item_key))


_SCHEMA_VERSION = next(
    f.default for f in dataclasses.fields(PartDescription) if f.name == "schema_version"
)


def dump_json(description: PartDescription, *, indent: int | None = None) -> str:
    """Compact JSON of @p description, schema_version always the current format constant."""
    mapping = to_mapping(description)
    assert isinstance(mapping, dict)
    mapping["schema_version"] = _SCHEMA_VERSION
    if indent is None:
        return json.dumps(mapping, separators=(",", ":"), ensure_ascii=True)
    return json.dumps(mapping, indent=indent, ensure_ascii=True)


def load_json(text: str, *, file: str) -> PartDescription:
    """PartDescription of @p text, DescriptionError when it is not valid JSON or does not match
    the schema."""
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise DescriptionError(file, exc.lineno, "<root>", "valid JSON") from exc
    return from_mapping(PartDescription, data, file=file)
