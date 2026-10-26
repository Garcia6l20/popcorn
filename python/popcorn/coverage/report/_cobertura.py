"""The Cobertura xml of a coverage report."""

from __future__ import annotations

import importlib.metadata
import os
import re
from collections import defaultdict
from decimal import Decimal
from pathlib import Path, PurePosixPath
from xml.etree import ElementTree

from popcorn.coverage.report._model import (
    BranchRecord,
    Counts,
    LcovFile,
    LcovReport,
    Totals,
    display_name,
)


def _version() -> str:
    try:
        return importlib.metadata.version("popcorn")
    except importlib.metadata.PackageNotFoundError:
        return "unknown"


_ILLEGAL = re.compile("[^\t\n\r\x20-\ud7ff\ue000-\ufffd\U00010000-\U0010ffff]")


def _clean(text: str) -> str:
    return _ILLEGAL.sub("", text)


def _rate(counts: Counts) -> str:
    if counts.found == 0:
        return "1.0"
    return format(Decimal(repr(counts.hit / counts.found)), "f")


def _rates(element: ElementTree.Element, totals: Totals) -> None:
    element.set("line-rate", _rate(totals.lines))
    element.set("branch-rate", _rate(totals.branches))


def _condition(branches: list[BranchRecord]) -> str:
    taken = sum(1 for branch in branches if branch.taken)
    return f"{100 * taken // len(branches)}% ({taken}/{len(branches)})"


def _class(file: LcovFile, filename: str) -> ElementTree.Element:
    element = ElementTree.Element(
        "class", name=_clean(PurePosixPath(filename).name), filename=_clean(filename)
    )
    _rates(element, file.totals())
    element.set("complexity", "0")
    methods = ElementTree.SubElement(element, "methods")
    for function in file.functions:
        method = ElementTree.SubElement(methods, "method", name=_clean(function.name), signature="")
        method.set("line-rate", "1.0" if function.hits else "0.0")
        method.set("branch-rate", "0.0")
        lines = ElementTree.SubElement(method, "lines")
        ElementTree.SubElement(lines, "line", number=str(function.start), hits=str(function.hits))
    by_line: dict[int, list[BranchRecord]] = defaultdict(list)
    for branch in file.branches:
        by_line[branch.line].append(branch)
    lines = ElementTree.SubElement(element, "lines")
    for number in sorted(file.lines):
        line = ElementTree.SubElement(
            lines, "line", number=str(number), hits=str(file.lines[number])
        )
        branches = by_line.get(number)
        if branches:
            line.set("branch", "true")
            line.set("condition-coverage", _condition(branches))
        else:
            line.set("branch", "false")
    return element


def render_cobertura(report: LcovReport, *, root: Path, timestamp: int) -> str:
    """The Cobertura xml of `report`, as a string with an xml declaration.

    `<sources>` lists `root` made absolute, and `/` too when a file lies outside it. A
    `<package>` holds the files of one directory, named by the directory of the `filename`. A
    `filename` is a relative `SF` path as written, an absolute one under `root` relative to it,
    and any other absolute path as written. Characters outside the xml 1.0 range are dropped
    from every name and path. `timestamp` is in milliseconds since the epoch.

    A `<method>` is a function: its `line` carries the function's start line and hits, its
    `line-rate` is 1.0 for a hit function and 0.0 otherwise, its `branch-rate` is 0.0. A branch
    line reports `condition-coverage` as the percent of its taken branches, cut to an integer.
    A line holds a `DA` record, so a `BRDA` on a line with no `DA` counts in the branch rates
    and appears on no line.
    """
    base = os.path.abspath(root)
    packages: dict[str, list[ElementTree.Element]] = defaultdict(list)
    totals: dict[str, Totals] = defaultdict(Totals)
    outside = False
    for file in report.files:
        filename = display_name(file.path, root)
        outside = outside or Path(filename).is_absolute()
        directory = PurePosixPath(filename).parent.as_posix()
        packages[directory].append(_class(file, filename))
        totals[directory] += file.totals()
    whole = report.totals()
    top = ElementTree.Element("coverage")
    _rates(top, whole)
    for name, counts in (
        ("lines-covered", whole.lines.hit),
        ("lines-valid", whole.lines.found),
        ("branches-covered", whole.branches.hit),
        ("branches-valid", whole.branches.found),
    ):
        top.set(name, str(counts))
    top.set("complexity", "0")
    top.set("version", _version())
    top.set("timestamp", str(timestamp))
    sources = ElementTree.SubElement(top, "sources")
    for source in (base, "/") if outside else (base,):
        ElementTree.SubElement(sources, "source").text = _clean(source)
    wrapper = ElementTree.SubElement(top, "packages")
    for directory in sorted(packages):
        package = ElementTree.SubElement(wrapper, "package", name=_clean(directory))
        _rates(package, totals[directory])
        package.set("complexity", "0")
        classes = ElementTree.SubElement(package, "classes")
        classes.extend(packages[directory])
    ElementTree.indent(top)
    body = ElementTree.tostring(top, encoding="unicode")
    return f'<?xml version="1.0" ?>\n{body}\n'
