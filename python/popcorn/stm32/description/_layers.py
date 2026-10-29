"""Layered form of shipped descriptions: family blocks, line manifest and part deltas.

A block is one peripheral's `registers` and `cross_rules`, keyed by the SHA-256 of its canonical
JSON, sorted keys and no spaces. A header is the rest of a peripheral, with `"block": <key>` in
place of `registers`. Three files hold the layers of the parts of one line:

- `<family>.blocks.json.xz`: `{"format", "family", "blocks": {key: block}}`, every block of the
  family's shipped parts.
- `<line>.line.json.xz`: `{"format", "line", "family", "reference", "schema_version", "core",
  "irqs", "peripherals": [header, ...]}`, the reference part's values.
- `<line>.parts.json.xz`: `{"format", "line", "family", "line_sha256", "parts": {name: delta}}`,
  `line_sha256` the SHA-256 of the decompressed line file. A delta holds `part`, `generator`,
  `sources` and `regions`, the headers that differ from the line's by instance name, and
  `schema_version`, `core` and `irqs` when they differ.

Family name: RefName[:7] + "xx". Line name: RefName[:9] + "xx". Every function here is pure and
imports only the standard library and `_errors`, so the build probe can run it without the
extension.

"""

import hashlib
import json
import lzma
from collections.abc import Mapping
from typing import Any, cast

from popcorn.stm32.description._errors import DescriptionError

LAYER_FORMAT = 1
"""Format number of every layer file, independent of the description's schema_version."""

TOP_KEYS = (
    "schema_version",
    "part",
    "generator",
    "sources",
    "core",
    "regions",
    "peripherals",
    "irqs",
)
"""Keys of a description in the order dump_json writes them."""

BLOCK_KEYS = ("registers", "cross_rules")
"""Peripheral keys held in a block, adjacent in a peripheral, `registers` first."""

DELTA_OPTIONAL = ("schema_version", "core", "irqs")
"""Description keys a delta holds only where they differ from the line's."""

_LAYER_SUFFIXES = {".blocks.json.xz": False, ".line.json.xz": True, ".parts.json.xz": True}

Json = Any


def dump(obj: Json) -> str:
    """Compact JSON of @p obj, as dump_json writes it."""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True)


def canonical(obj: Json) -> str:
    """Canonical JSON of @p obj: sorted keys, no spaces."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def block_key(block: Json) -> str:
    """Hex SHA-256 of the canonical JSON of @p block."""
    return hashlib.sha256(canonical(block).encode("ascii")).hexdigest()


def pack(obj: Json) -> bytes:
    """xz bytes of the compact JSON of @p obj, preset 9 with a CRC64 check. Equal objects give
    equal bytes."""
    return lzma.compress(
        dump(obj).encode("ascii"), format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=9
    )


def refname(name: str) -> str:
    """RefName of the shipped name @p name, `<RefName>[-<core>]`."""
    return name.partition("-")[0]


def family_name(name: str) -> str:
    """Family of the shipped name @p name, `STM32U5xx` for `STM32U575QGIx`."""
    return refname(name)[:7] + "xx"


def line_name(name: str) -> str:
    """Line of the shipped name @p name, `STM32U575xx` for `STM32U575QGIx`."""
    return refname(name)[:9] + "xx"


def file_names(line: str, family: str) -> tuple[str, str, str]:
    """File names of the parts, line and family layers of @p line in @p family."""
    return f"{line}.parts.json.xz", f"{line}.line.json.xz", f"{family}.blocks.json.xz"


def layer_family(file: str) -> str | None:
    """Family whose layers include the file named @p file, None for a name that is no layer
    file: `STM32U5xx` for `STM32U5xx.blocks.json.xz` and for `STM32U575xx.line.json.xz`."""
    for suffix, of_line in _LAYER_SUFFIXES.items():
        if file.endswith(suffix):
            stem = file.removesuffix(suffix)
            return family_name(stem) if of_line else stem
    return None


def choose_reference(ranks: Mapping[str, tuple[int, int]]) -> dict[str, str]:
    """Line to reference part of the shipped names in @p ranks, each mapped to its bonded GPIO
    pin count and flash size in KiB: per line the part with the most bonded pins, then the
    largest flash, then the first name in sorted order."""
    references: dict[str, str] = {}
    for name in sorted(ranks, key=lambda name: (-ranks[name][0], -ranks[name][1], name)):
        references.setdefault(line_name(name), name)
    return dict(sorted(references.items()))


def _split(name: str, description: Mapping[str, Json]) -> tuple[list[Json], dict[str, Json]]:
    if tuple(description) != TOP_KEYS:
        raise DescriptionError(name, 0, "<root>", f"the keys {', '.join(TOP_KEYS)} in order")
    headers: list[Json] = []
    blocks: dict[str, Json] = {}
    for peripheral in cast(list[dict[str, Json]], description["peripherals"]):
        keys = list(peripheral)
        if "registers" not in keys or keys[keys.index("registers") + 1 :][:1] != ["cross_rules"]:
            raise DescriptionError(
                name, 0, f"peripherals.{peripheral.get('name')}", "registers then cross_rules"
            )
        block = {key: peripheral[key] for key in BLOCK_KEYS}
        key = block_key(block)
        blocks.setdefault(key, block)
        header: dict[str, Json] = {}
        for field, value in peripheral.items():
            if field == "registers":
                header["block"] = key
            elif field not in BLOCK_KEYS:
                header[field] = value
        headers.append(header)
    return headers, blocks


def factor(
    descriptions: Mapping[str, Mapping[str, Json]], references: Mapping[str, str]
) -> dict[str, bytes]:
    """Layer files of @p descriptions, keyed by shipped name, `<RefName>[-<core>]`, each a
    description mapping as dump_json writes it. @p references maps each line to the name of its
    reference part, one of @p descriptions.

    Returns file name to xz bytes: one blocks file per family, one line and one parts file per
    line, parts sorted by name. Equal inputs give equal bytes. DescriptionError naming the part
    when a description's top-level keys are not TOP_KEYS in order, when a line has no reference
    among @p descriptions, or when a part's instance names or order differ from its reference's.
    """
    split = {name: _split(name, descriptions[name]) for name in sorted(descriptions)}
    members: dict[str, list[str]] = {}
    for name in split:
        members.setdefault(line_name(name), []).append(name)
    family_blocks: dict[str, dict[str, Json]] = {}
    for name, (_, blocks) in split.items():
        family_blocks.setdefault(family_name(name), {}).update(blocks)
    files: dict[str, bytes] = {}
    for family, blocks in sorted(family_blocks.items()):
        ordered = {key: blocks[key] for key in sorted(blocks)}
        files[f"{family}.blocks.json.xz"] = pack(
            {"format": LAYER_FORMAT, "family": family, "blocks": ordered}
        )
    for line, names in sorted(members.items()):
        reference = references.get(line)
        if reference is None or reference not in split or line_name(reference) != line:
            raise DescriptionError(names[0], 0, "reference", f"a reference part of {line}")
        family = family_name(reference)
        base = descriptions[reference]
        line_headers = split[reference][0]
        line_doc: dict[str, Json] = {
            "format": LAYER_FORMAT,
            "line": line,
            "family": family,
            "reference": reference,
            "schema_version": base["schema_version"],
            "core": base["core"],
            "irqs": base["irqs"],
            "peripherals": line_headers,
        }
        line_text = dump(line_doc)
        by_name = {header["name"]: header for header in line_headers}
        line_order = [header["name"] for header in line_headers]
        deltas: dict[str, Json] = {}
        for name in names:
            description = descriptions[name]
            headers = split[name][0]
            if [header["name"] for header in headers] != line_order:
                raise DescriptionError(
                    name, 0, "peripherals", f"the instances of {reference} in its order"
                )
            delta: dict[str, Json] = {
                "part": description["part"],
                "generator": description["generator"],
                "sources": description["sources"],
                "regions": description["regions"],
                "peripherals": {
                    header["name"]: header
                    for header in headers
                    if canonical(header) != canonical(by_name[header["name"]])
                },
            }
            for key in DELTA_OPTIONAL:
                if canonical(description[key]) != canonical(line_doc[key]):
                    delta[key] = description[key]
            deltas[name] = delta
        parts_name, line_file, _ = file_names(line, family)
        files[line_file] = pack(line_doc)
        files[parts_name] = pack(
            {
                "format": LAYER_FORMAT,
                "line": line,
                "family": family,
                "line_sha256": hashlib.sha256(line_text.encode("ascii")).hexdigest(),
                "parts": deltas,
            }
        )
    return files


def _object(value: Json, file: str, key: str) -> dict[str, Json]:
    if not isinstance(value, dict):
        raise DescriptionError(file, 0, key, "an object")
    return cast(dict[str, Json], value)


def compose(
    name: str,
    parts: Mapping[str, Json],
    line: Mapping[str, Json],
    family: Mapping[str, Json],
    *,
    limit: int,
    files: tuple[str, str, str] = ("parts", "line", "family"),
) -> dict[str, Json]:
    """Description mapping of the part @p name, its delta in the parts layer @p parts over the
    line layer @p line over the blocks of the family layer @p family, keys in TOP_KEYS order and
    instances in the line's order.

    @p files names the parts, line and family files in errors. The caller checks each layer's
    `format` and the parts layer's `line_sha256`. DescriptionError naming the file when the
    layers name another line or family, @p name has no delta, a delta names an instance the line
    lacks, a block is missing or its bytes do not hash to its key, or the composed JSON would
    exceed @p limit bytes. The size is counted before any peripheral is built, from each
    referenced block's length counted once, so one call's work is bounded by @p limit and the
    layer sizes whatever the reference pattern."""
    parts_file, line_file, family_file = files
    line_id = line.get("line")
    family_id = line.get("family")
    if parts.get("line") != line_id:
        raise DescriptionError(parts_file, 0, "line", f"line {line_id}")
    if parts.get("family") != family_id or family.get("family") != family_id:
        raise DescriptionError(family_file, 0, "family", f"family {family_id}")
    deltas = _object(parts.get("parts"), parts_file, "parts")
    if name not in deltas:
        raise DescriptionError(parts_file, 0, f"parts.{name}", "a delta for the part")
    delta = _object(deltas[name], parts_file, f"parts.{name}")
    overrides = _object(delta.get("peripherals"), parts_file, f"parts.{name}.peripherals")
    missing = [key for key in ("part", "generator", "sources", "regions") if key not in delta]
    if missing:
        raise DescriptionError(parts_file, 0, f"parts.{name}.{missing[0]}", "a delta key")
    blocks = _object(family.get("blocks"), family_file, "blocks")
    line_headers = line.get("peripherals")
    if not isinstance(line_headers, list) or any(key not in line for key in DELTA_OPTIONAL):
        raise DescriptionError(line_file, 0, "<root>", "a line layer")
    line_headers = cast(list[dict[str, Json]], line_headers)
    known = {header["name"] for header in line_headers}
    unknown = sorted(set(overrides) - known)
    if unknown:
        raise DescriptionError(
            parts_file, 0, f"parts.{name}.peripherals.{unknown[0]}", f"an instance of {line_id}"
        )
    headers = [overrides.get(header["name"], header) for header in line_headers]
    sizes: dict[str, int] = {}
    total = len(dump(delta)) + sum(len(dump(line[key])) for key in DELTA_OPTIONAL)
    for header in headers:
        key = header.get("block")
        if key not in sizes:
            if not isinstance(key, str) or key not in blocks:
                raise DescriptionError(family_file, 0, f"blocks.{key}", "the block of a header")
            text = canonical(blocks[key])
            if hashlib.sha256(text.encode("ascii")).hexdigest() != key:
                raise DescriptionError(family_file, 0, f"blocks.{key}", "a block hashing to key")
            sizes[key] = len(text)
        total += sizes[key] + len(dump(header))
        if total > limit:
            raise DescriptionError(
                parts_file, 0, f"parts.{name}", f"a composed description of at most {limit} bytes"
            )
    peripherals: list[Json] = []
    for header in headers:
        peripheral: dict[str, Json] = {}
        for field, value in header.items():
            if field == "block":
                peripheral.update(blocks[value])
            else:
                peripheral[field] = value
        peripherals.append(peripheral)
    composed: dict[str, Json] = {}
    for key in TOP_KEYS:
        if key == "peripherals":
            composed[key] = peripherals
        elif key in DELTA_OPTIONAL:
            composed[key] = delta.get(key, line[key])
        else:
            composed[key] = delta[key]
    return composed
