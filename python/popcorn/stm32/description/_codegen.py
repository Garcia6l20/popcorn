"""Generates ``_model.py`` from ``data/schema/mcu_description.schema.json``.

Regenerate with ``python -m popcorn.stm32.description._codegen <schema> <out>``.
"""

import json
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import cast

type JsonObject = Mapping[str, object]

_HEADER = '''"""Dataclasses and enums generated from data/schema/mcu_description.schema.json.

Regenerate with ``python -m popcorn.stm32.description._codegen``. Do not edit by hand.
"""

from dataclasses import dataclass, field
from enum import StrEnum

'''


def _as_object(value: object) -> JsonObject:
    assert isinstance(value, Mapping)
    return cast(JsonObject, value)


def _maybe_object(value: object) -> JsonObject | None:
    if isinstance(value, Mapping):
        return cast(JsonObject, value)
    return None


def _pascal_case(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def _ref_name(ref: str) -> str:
    return ref.rsplit("/", 1)[-1]


def _collect_refs(node: JsonObject) -> set[str]:
    refs: set[str] = set()
    ref = node.get("$ref")
    if isinstance(ref, str):
        refs.add(_ref_name(ref))
    if node.get("type") == "array":
        refs |= _collect_refs(_as_object(node.get("items")))
    additional = _maybe_object(node.get("additionalProperties"))
    if additional is not None:
        refs |= _collect_refs(additional)
    return refs


def _def_refs(def_node: JsonObject) -> set[str]:
    refs: set[str] = set()
    for prop in _as_object(def_node.get("properties", {})).values():
        refs |= _collect_refs(_as_object(prop))
    return refs


def _topo_sort(defs: Mapping[str, JsonObject]) -> list[str]:
    order = list(defs)
    deps = {name: _def_refs(node) & defs.keys() for name, node in defs.items()}
    sorted_names: list[str] = []
    seen: set[str] = set()

    def visit(name: str) -> None:
        if name in seen:
            return
        seen.add(name)
        for dep in sorted(deps[name]):
            visit(dep)
        sorted_names.append(name)

    for name in order:
        visit(name)
    return sorted_names


def _py_type(node: JsonObject) -> str:
    ref = node.get("$ref")
    if isinstance(ref, str):
        return _pascal_case(_ref_name(ref))
    node_type = node.get("type")
    if node_type == "string":
        return "str"
    if node_type == "boolean":
        return "bool"
    if node_type == "integer":
        return "int"
    if node_type == "array":
        items = node.get("items")
        return f"list[{_py_type(_as_object(items))}]"
    if node_type == "object":
        additional = node.get("additionalProperties")
        return f"dict[str, {_py_type(_as_object(additional))}]"
    if node_type is None and "const" in node:
        return "int"
    raise ValueError(f"unsupported schema node: {node!r}")


def _bounds_metadata(node: JsonObject) -> dict[str, int]:
    metadata: dict[str, int] = {}
    minimum = node.get("minimum")
    maximum = node.get("maximum")
    if isinstance(minimum, int):
        metadata["minimum"] = minimum
    if isinstance(maximum, int):
        metadata["maximum"] = maximum
    return metadata


def _emit_enum(name: str, node: JsonObject) -> str:
    values = node["enum"]
    assert isinstance(values, list)
    lines = [f"class {_pascal_case(name)}(StrEnum):"]
    for value in cast(list[object], values):
        assert isinstance(value, str)
        lines.append(f'    {value.upper()} = "{value}"')
    return "\n".join(lines)


def _emit_field(prop_name: str, prop_node: JsonObject, *, required: bool) -> str:
    py_type = _py_type(prop_node)
    const = prop_node.get("const")
    metadata = _bounds_metadata(prop_node)
    if const is not None:
        assert isinstance(const, int)
        return (
            f"    {prop_name}: {py_type} = field(default={const}, "
            f'metadata={{"const": {const}}})'
        )
    if not required:
        if metadata:
            metadata_repr = ", ".join(f'"{k}": {v}' for k, v in metadata.items())
            return (
                f"    {prop_name}: {py_type} | None = "
                f"field(default=None, metadata={{{metadata_repr}}})"
            )
        return f"    {prop_name}: {py_type} | None = None"
    if metadata:
        metadata_repr = ", ".join(f'"{k}": {v}' for k, v in metadata.items())
        return f"    {prop_name}: {py_type} = field(metadata={{{metadata_repr}}})"
    return f"    {prop_name}: {py_type}"


def _emit_dataclass(name: str, node: JsonObject) -> str:
    properties = _as_object(node.get("properties"))
    required = node.get("required")
    required_names: list[str] = (
        list(cast(list[str], required)) if isinstance(required, list) else []
    )
    lines = ["@dataclass(slots=True, kw_only=True)", f"class {_pascal_case(name)}:"]
    for prop_name in required_names:
        prop_node = _as_object(properties[prop_name])
        lines.append(_emit_field(prop_name, prop_node, required=True))
    for prop_name, prop_value in properties.items():
        if prop_name in required_names:
            continue
        lines.append(_emit_field(prop_name, _as_object(prop_value), required=False))
    return "\n".join(lines)


def generate(schema: JsonObject) -> str:
    """Python source of the description dataclasses and enums for @p schema."""
    defs_raw = _as_object(schema.get("$defs"))
    defs = {name: _as_object(node) for name, node in defs_raw.items()}
    enum_names = {name for name, node in defs.items() if "enum" in node}
    order = _topo_sort(defs)

    parts = [_HEADER.rstrip("\n")]
    for name in order:
        node = defs[name]
        if name in enum_names:
            parts.append(_emit_enum(name, node))
        else:
            parts.append(_emit_dataclass(name, node))
    parts.append(_emit_dataclass("mcu_description", schema))

    return "\n\n\n".join(parts) + "\n"


def main(argv: Sequence[str]) -> int:
    if len(argv) != 2:
        print("usage: python -m popcorn.stm32.description._codegen <schema> <out>", file=sys.stderr)
        return 2
    schema_path, out_path = argv
    schema = _as_object(json.loads(Path(schema_path).read_text(encoding="utf-8")))
    Path(out_path).write_text(generate(schema), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
