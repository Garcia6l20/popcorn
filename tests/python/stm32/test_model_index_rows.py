"""The model index of `docs/dev/peripherals.md` agrees with the built row tables."""

from __future__ import annotations

import re
from pathlib import Path

from popcorn import _core

PAGE = Path(__file__).parents[3] / "docs/dev/peripherals.md"
FAMILIES = ("u5", "h5", "l1", "l4")
NAME = re.compile(r"`([^`]+)`")


def index() -> dict[str, tuple[list[str], dict[str, list[str]]]]:
    text = PAGE.read_text()
    start = text.index("### Models")
    body = text[start : text.index("\n### ", start + 1)]
    found: dict[str, tuple[list[str], dict[str, list[str]]]] = {}
    for line in body.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 7 or cells[0] in {"IP group", "---"}:
            continue
        found[cells[0]] = (
            NAME.findall(cells[1]),
            {family: NAME.findall(cell) for family, cell in zip(FAMILIES, cells[2:6], strict=True)},
        )
    return found


def rows() -> list[tuple[str, tuple[str, ...]]]:
    return [(row.label.split("/")[0], tuple(row.families)) for row in _core.model_rows()]


def group_of(ip: str) -> str:
    owners = [group for group, (ips, _) in index().items() if ip in ips]
    assert len(owners) == 1, f"{ip} is listed by {owners}"
    return owners[0]


def test_every_ip_of_the_row_tables_is_listed_in_one_group() -> None:
    for ip in {ip for ip, _ in rows()}:
        group_of(ip)


def test_every_row_family_has_a_model_in_the_index() -> None:
    cells = index()
    empty = [
        (ip, family)
        for ip, families in rows()
        for family in families
        if not cells[group_of(ip)][1][family]
    ]
    assert not empty


def test_every_model_cell_has_a_row() -> None:
    served = {(group_of(ip), family) for ip, families in rows() for family in families}
    stale = [
        (group, family)
        for group, (_, cells) in index().items()
        for family, cell in cells.items()
        if cell and (group, family) not in served
    ]
    assert not stale


def test_every_listed_ip_string_has_a_row() -> None:
    present = {ip for ip, _ in rows()}
    unused = [ip for ips, _ in index().values() for ip in ips if ip not in present]
    assert not unused
