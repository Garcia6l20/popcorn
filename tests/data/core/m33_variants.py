"""Test descriptors derived from `m33.json`, by `m33_variants.py BASE OUT_DIR [NAME...]`.

Each variant is the base part plus a delta. `OUT_DIR` receives one `<name>.json` per NAME, or per
entry of VARIANTS without one, in the layout `json.dumps(indent=2)` gives. The build rule passes
the names it declares as outputs, `M33_VARIANT_NAMES` of `popcorn_build/testing.py`.
"""

from __future__ import annotations

import copy
import json
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import cast

type Json = dict[str, object]

SECURE_ALIAS_OFFSET = 0x1000_0000
FULL_MASK = 0xFFFF_FFFF


def _list(part: Json, key: str) -> list[Json]:
    return cast(list[Json], part[key])


def _value(name: str, offset: int, reset: int) -> Json:
    """A 32-bit read-write register of one VAL field, as the shared-page peripherals have."""
    return {
        "name": name,
        "offset": offset,
        "width": 32,
        "reset": reset,
        "reset_mask": FULL_MASK,
        "acc": "read_write",
        "fields": [
            {
                "name": "VAL",
                "lsb": 0,
                "width": 32,
                "acc": "read_write",
                "on_write": "modify",
                "on_read": "none",
            }
        ],
    }


def _peripheral(
    name: str,
    base: int,
    size: int,
    registers: list[Json],
    irqs: list[int] | None = None,
    ip: str | None = None,
) -> Json:
    return {
        "name": name,
        "ip": ip or name.lower(),
        "ip_version": "v1",
        "base": base,
        "size": size,
        "registers": registers,
        "cross_rules": [],
        "irqs": irqs or [],
    }


def _secure_alias(peripheral: Json) -> Json:
    return {
        "name": f"SEC_{peripheral['name']}",
        "ip": peripheral["ip"],
        "ip_version": peripheral["ip_version"],
        "base": cast(int, peripheral["base"]) + SECURE_ALIAS_OFFSET,
        "size": peripheral["size"],
        "registers": [],
        "cross_rules": [],
        "irqs": [],
        "alias_of": peripheral["name"],
    }


def _field(name: str, lsb: int, width: int, acc: str = "read_write") -> Json:
    return {
        "name": name,
        "lsb": lsb,
        "width": width,
        "acc": acc,
        "on_read": "none",
        "on_write": "modify",
    }


def _model_register(
    name: str, offset: int, fields: list[Json], width: int = 32, acc: str = "read_write"
) -> Json:
    """A register of the Python model peripherals, reset 0 over all of @p width."""
    return {
        "name": name,
        "offset": offset,
        "width": width,
        "acc": acc,
        "reset": 0,
        "reset_mask": (1 << width) - 1,
        "fields": fields,
    }


def code_alias(base: Json) -> Json:
    """SRAM loses execute and gains an execute-only alias at 0x0A00_0000."""
    part = copy.deepcopy(base)
    part["part"] = "POPCORN-M33-TEST-CODE-ALIAS"
    regions = _list(part, "regions")
    (sram,) = (region for region in regions if region["name"] == "SRAM")
    cast(Json, sram["acc"])["execute"] = False
    regions.append(
        {
            "name": "SRAM_CODE",
            "start": 0x0A00_0000,
            "size": sram["size"],
            "kind": "ram",
            "acc": {"read": True, "write": False, "execute": True},
            "secure": False,
            "physical": sram["start"],
        }
    )
    return part


def shared_page(base: Json) -> Json:
    """Five peripherals sharing and crossing 1 KiB pages, three of them with secure aliases."""
    part = copy.deepcopy(base)
    part["part"] = "POPCORN-M33-TEST-shared-page"
    pair = _peripheral(
        "PAIR", 0x4003_0000, 0xCC, [_value("CR", 0x0, 0x11), _value("SR", 0x4, 0x22)]
    )
    second = _peripheral("SECOND", 0x4003_0100, 0x10, [_value("DR", 0x0, 0x33)])
    common = _peripheral("COMMON", 0x4003_0300, 0xC, [_value("CCR", 0x8, 0x44)])
    wide = _peripheral(
        "WIDE", 0x4004_0000, 0x500, [_value("CR", 0x0, 0x55), _value("TAIL", 0x4FC, 0x66)]
    )
    last = _peripheral("LAST", 0x4004_0600, 0x8, [_value("DR", 0x0, 0x77)])
    _list(part, "peripherals").extend(
        [
            pair,
            second,
            common,
            wide,
            last,
            *(_secure_alias(peripheral) for peripheral in (pair, second, common)),
        ]
    )
    return part


def model(base: Json) -> Json:
    """The TIMER and EEPROM peripherals the Python model tests serve, TIMER on IRQ 4."""
    part = copy.deepcopy(base)
    timer_irq = 4
    timer = _peripheral(
        "TIMER",
        0x4003_0000,
        0x400,
        [
            _model_register("CR", 0x0, [_field("EN", 0, 1)]),
            _model_register("ARR", 0x4, [_field("VAL", 0, 32)]),
            _model_register("SR", 0x8, [_field("UIF", 0, 1)]),
            _model_register("CNT", 0xC, [_field("VAL", 0, 32, "read_only")], acc="read_only"),
        ],
        irqs=[timer_irq],
        ip="pytimer",
    )
    eeprom = _peripheral(
        "EEPROM",
        0x4004_0000,
        0x400,
        [
            _model_register("CR", 0x0, [_field("START", 0, 1), _field("STOP", 1, 1)]),
            _model_register("DR", 0x4, [_field("DATA", 0, 8)]),
            _model_register("ADDR", 0x8, [_field("VAL", 0, 16)], width=16),
            _model_register("LEN", 0xA, [_field("VAL", 0, 16)], width=16),
        ],
        ip="pyeeprom",
    )
    _list(part, "peripherals").extend([timer, eeprom])
    (irq,) = (irq for irq in _list(part, "irqs") if irq["number"] == timer_irq)
    irq["name"] = "TIMER_IRQn"
    return part


VARIANTS: dict[str, Callable[[Json], Json]] = {
    "m33_code_alias": code_alias,
    "m33_shared_page": shared_page,
    "model": model,
}


def write_all(base: Path, out_dir: Path, names: Sequence[str] = ()) -> None:
    """Write the variants @p names of the part at @p base into @p out_dir as `<name>.json`.

    Every variant of VARIANTS when @p names is empty. A file that already holds the variant is
    left untouched, so the build rule's restat skips the programs that depend on it.

    @throws KeyError A name that VARIANTS lacks.
    """
    derivations = {name: VARIANTS[name] for name in names} if names else VARIANTS
    part = cast(Json, json.loads(base.read_text(encoding="utf-8")))
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, derive in derivations.items():
        text = json.dumps(derive(part), indent=2) + "\n"
        out = out_dir / f"{name}.json"
        if not out.is_file() or out.read_text(encoding="utf-8") != text:
            out.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    write_all(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:])
