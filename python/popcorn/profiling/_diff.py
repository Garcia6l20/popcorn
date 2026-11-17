"""Differences between two profile reports."""

from __future__ import annotations

import dataclasses
import typing
from collections import defaultdict
from collections.abc import Iterable
from typing import TYPE_CHECKING, Any, cast

if TYPE_CHECKING:
    from popcorn.profiling._report import Report

type Value = int | float | None
type Row = dict[str, Value]
type Table = dict[str, Row]
type ReportDiff = dict[str, Table]
"""What `diff` returns: table name, then row key, then field name to its change."""

MEASURES: tuple[object, ...] = (int, float, int | None, float | None)
"""Annotations of the fields a diff compares."""
IDENTITY = frozenset(
    {"start", "parent", "key", "number", "address", "last_value", "head", "last", "closest_pc"}
)
"""Numeric fields that place or name a row rather than measure it, left out of a diff."""


def _add(table: dict[str, dict[str, Value]], key: str, row: dict[str, Value]) -> None:
    held = table.setdefault(key, {})
    for name, value in row.items():
        before = held.get(name)
        held[name] = value if before is None or value is None else before + value


def _rows(items: Iterable[tuple[str, dict[str, Value]]]) -> Table:
    table: dict[str, dict[str, Value]] = {}
    for key, row in items:
        _add(table, key, row)
    return table


def _measures(row: object, *extra: tuple[str, Value]) -> dict[str, Value]:
    hints = _hints(type(row))
    return {
        field.name: getattr(row, field.name)
        for field in dataclasses.fields(cast(Any, row))
        if hints[field.name] in MEASURES and field.name not in IDENTITY
    } | dict(extra)


def _hints(kind: type) -> dict[str, Any]:
    return typing.get_type_hints(kind)


def _tables(report: Report) -> dict[str, Table]:
    tables: dict[str, Table] = {"summary": {"report": _measures(report)}}
    if report.hotspots is not None:
        tables["functions"] = _rows((row.name, _measures(row)) for row in report.hotspots.functions)
        paths: list[str] = []
        frames: list[tuple[str, dict[str, Value]]] = []
        for index, node in enumerate(report.hotspots.frames):
            own = f"{node.kind}:{node.name}"
            paths.append(own if index == 0 else f"{paths[node.parent]};{own}")
            frames.append((paths[index], _measures(node)))
        tables["frames"] = _rows(frames)
    if report.sleep is not None:
        sleep = report.sleep
        tables["sleep"] = {
            "sleep": _measures(
                sleep, *((f"reason_{name}", count) for name, count in sleep.reasons.items())
            )
        }
        tables["wakes"] = _rows((wake.name, _measures(wake)) for wake in sleep.wakes)
    if report.irq is not None:
        tables["exceptions"] = _rows((row.name, _measures(row)) for row in report.irq.exceptions)
    if report.peripherals is not None:
        tables["registers"] = _rows(
            (f"{initiator}:{row.name}", _measures(row))
            for initiator, rows in (
                ("cpu", report.peripherals.cpu),
                ("dma", report.peripherals.dma),
            )
            for row in rows
        )
    if report.loops is not None:
        tables["loops"] = _rows((row.location, _measures(row)) for row in report.loops.loops)
    if report.watchdog is not None:
        dog = report.watchdog
        tables["watchdog"] = {"watchdog": _measures(dog, ("gaps", len(dog.gaps)))}
    if report.timeline is not None:
        tables["timeline"] = _rows(
            (milestone.name, _measures(milestone)) for milestone in report.timeline
        )
    return tables


def diff(a: Report, b: Report) -> ReportDiff:
    """What changed from `a` to `b`, table by table.

    Tables are `summary`, `functions` by name, `frames` by the path of `kind:name` from the
    root, `sleep`, `wakes`, `exceptions` and `loops` by name, `registers` by `cpu:` or `dma:`
    and name, `watchdog` and `timeline` by milestone name, each present when either report
    has the section. A row maps each changed field to `b` less `a`, a row present in one report
    only counts the other as 0. A field that is None in one report only maps to the other's
    value, or to None when `b` lost it. Rows and tables with no change are left out, so a
    report against itself gives an empty dict. Rows with one name in one report are summed.
    """
    left, right = _tables(a), _tables(b)
    out: dict[str, Table] = {}
    for name in left.keys() | right.keys():
        before, after = left.get(name, {}), right.get(name, {})
        changed: Table = defaultdict(dict)
        for key in before.keys() | after.keys():
            old, new = before.get(key, {}), after.get(key, {})
            for field in old.keys() | new.keys():
                was, now = old.get(field, 0), new.get(field, 0)
                if was is None or now is None:
                    if was is not now:
                        changed[key][field] = now
                elif now != was:
                    changed[key][field] = now - was
        if changed:
            out[name] = dict(changed)
    return out
