"""IRQ table: SVD interrupt values reconciled with the CMSIS IRQn_Type enum and the IRQ patch
rules, plus each peripheral's own IRQ numbers."""

import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import IrqDesc
from popcorn.stm32.description._patches import PatchIrqs
from popcorn.stm32.description._svd import SvdDevice

_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_ENUM_RE = re.compile(r"typedef\s+enum\s*\{(.*?)\}\s*IRQn_Type\s*;", re.DOTALL)
_DIRECTIVE_RE = re.compile(r"^\s*#\s*\w+", re.MULTILINE)
_NUMERIC_RE = re.compile(r"^-?(?:0[xX][0-9A-Fa-f]+|[0-9]+)$")


@dataclass(slots=True, frozen=True, kw_only=True)
class IrqTable:
    irqs: list[IrqDesc]
    by_peripheral: dict[str, list[int]]
    num_irqs: int
    notes: list[str]


def read_cmsis_irqs(text: str, *, file: str) -> dict[int, str]:
    """External IRQs of the IRQn_Type enum, number -> enumerator name with _IRQn, core entries
    (negative numbers) dropped. DescriptionError when the enum body contains a preprocessor
    directive (some headers use #if/#elif branches to name the same number differently per
    variant, and the last branch would otherwise silently win), when an enumerator aliases
    another name instead of a numeric value (NAME = OTHER_IRQn, seen in no installed header but
    not rejected by a bare value regex either), or when two different names claim one number."""
    stripped = _COMMENT_RE.sub(" ", text)
    match = _ENUM_RE.search(stripped)
    if match is None:
        raise DescriptionError(file, 0, "IRQn_Type", "a typedef enum { ... } IRQn_Type; block")
    body = match.group(1)
    if _DIRECTIVE_RE.search(body):
        raise DescriptionError(file, 0, "IRQn_Type", "no preprocessor directive in the enum body")

    result: dict[int, str] = {}
    for entry in body.split(","):
        entry = entry.strip()
        if not entry:
            continue
        name, sep, rhs = entry.partition("=")
        name = name.strip()
        rhs = rhs.strip()
        if not sep:
            raise DescriptionError(file, 0, f"IRQn_Type.{name}", "NAME = value")
        if not _NUMERIC_RE.match(rhs):
            raise DescriptionError(file, 0, f"IRQn_Type.{name}", "a numeric value, not an alias")
        value = int(rhs, 0)
        if value < 0:
            continue
        existing = result.get(value)
        if existing is not None and existing != name:
            raise DescriptionError(
                file, 0, f"IRQn_Type[{value}]", f"one name, has {existing} and {name}"
            )
        result[value] = name
    return result


def build_irq_table(
    svd: SvdDevice,
    cmsis: Mapping[int, str] | None,
    patches: Sequence[PatchIrqs],
    rzone_num: int | None,
) -> IrqTable:
    """IrqTable of @p svd, reconciled against @p cmsis (None when no header was found) and
    @p patches (drop by SVD name, then add), DescriptionError on a duplicate SVD number, an
    unknown drop name, an add on a number already in use, or a CMSIS number no SVD interrupt or
    patch add covers."""
    numbers: dict[int, str] = {}
    for peripheral in svd.peripherals:
        for number, name in peripheral.interrupts:
            existing = numbers.get(number)
            if existing is not None and existing != name:
                raise DescriptionError(
                    "svd", 0, f"irqs[{number}]", f"one name, has {existing} and {name}"
                )
            numbers[number] = name

    for patch in patches:
        for name in patch.drop or []:
            matching = [n for n, nm in numbers.items() if nm == name]
            if not matching:
                raise DescriptionError("irqs", 0, f"drop.{name}", "a known SVD IRQ name")
            for number in matching:
                del numbers[number]

    for patch in patches:
        for add in patch.add or []:
            if add.number in numbers:
                raise DescriptionError("irqs", 0, f"add.{add.name}", "an unused IRQ number")
            numbers[add.number] = add.name

    if cmsis is not None:
        missing = sorted(set(cmsis) - set(numbers))
        if missing:
            names = ", ".join(f"{cmsis[n]}({n})" for n in missing)
            raise DescriptionError(
                "irqs", 0, "cmsis", f"an SVD interrupt or a patch add for {names}"
            )

    irqs = [
        IrqDesc(
            number=number,
            name=(
                cmsis[number]
                if cmsis is not None and number in cmsis
                else f"{numbers[number]}_IRQn"
            ),
        )
        for number in sorted(numbers)
    ]

    num_irqs = max(numbers) + 1 if numbers else 0
    notes: list[str] = []
    if rzone_num is not None and rzone_num != num_irqs:
        notes.append(f"rzone DnumInterrupts={rzone_num} differs from num_irqs={num_irqs}")

    by_peripheral: dict[str, list[int]] = {p.name: [] for p in svd.peripherals}
    for peripheral in svd.peripherals:
        for number, _ in peripheral.interrupts:
            if number in numbers:
                by_peripheral[peripheral.name].append(number)
    for patch in patches:
        for add in patch.add or []:
            by_peripheral.setdefault(add.peripheral, []).append(add.number)
    for name, peripheral_irqs in by_peripheral.items():
        by_peripheral[name] = sorted(set(peripheral_irqs))

    return IrqTable(irqs=irqs, by_peripheral=by_peripheral, num_irqs=num_irqs, notes=notes)
