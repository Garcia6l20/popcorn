"""The C integer constant expression evaluator of `Constants.macro`, on AAPCS ILP32."""

from __future__ import annotations

import pytest
from popcorn._c_expr import UCHAR, CType, CValue, evaluate

NAMES = {"FOUR": CValue(4, CType(32, True, 2))}
TYPEDEFS = {"repeat_t": UCHAR}


def resolve(name: str) -> CValue:
    if name in NAMES:
        return NAMES[name]
    raise ValueError(f"{name} is unknown")


def value(text: str) -> int:
    return evaluate(text, resolve, TYPEDEFS.get).value


CASES = [
    ("16", 16),
    ("0x10u", 16),
    ("0b101", 5),
    ("017", 15),
    ("1'000", 1000),
    ("(1UL << 5)", 32),
    ("2147483648 - 1", 2147483647),
    ("0x80000000 >> 31", 1),
    ("2147483648 > 0", 1),
    ("-1L < 1U", 0),
    ("-1 < 1U", 0),
    ("-1LL < 1U", 1),
    ("1u - 2", 0xFFFF_FFFF),
    ("-1 >> 1", -1),
    ("1 ? -1 : 1u", 0xFFFF_FFFF),
    ("0 ? 1u : -1", 0xFFFF_FFFF),
    ("((repeat_t) - 1)", 255),
    ("((FOUR) - 1)", 3),
    ("(unsigned long) -1", 0xFFFF_FFFF),
    ("(long unsigned) -1", 0xFFFF_FFFF),
    ("(unsigned char) 300", 44),
    ("(signed char) 200", -56),
    ("(long long) -1 >> 40", -1),
    ("'\\xff'", 255),
    ("'A' + 1", 66),
    ("'\\n'", 10),
    ("0 && 1/0", 0),
    ("1 || 1/0", 1),
    ("1 ? 2 : 1/0", 2),
    ("~0u", 0xFFFF_FFFF),
    ("!5", 0),
    ("-7 / 2", -3),
    ("-7 % 2", -1),
    ("(0xF0u | 0x0Fu) ^ 0x0F", 0xF0),
    ("1 + 2 * 3 << 1", 14),
    ("FOUR * FOUR == 16", 1),
]


@pytest.mark.parametrize(("text", "expected"), CASES, ids=[text for text, _ in CASES])
def test_an_expression_gives_the_value_gcc_gives(text: str, expected: int) -> None:
    assert value(text) == expected


def test_literal_types_follow_c_on_ilp32() -> None:
    assert evaluate("2147483648", resolve, TYPEDEFS.get).type == CType(64, True, 4)
    assert evaluate("0x80000000", resolve, TYPEDEFS.get).type == CType(32, False, 2)
    assert evaluate("1UL", resolve, TYPEDEFS.get).type == CType(32, False, 3)


RAISING = [
    ("1/0", "division by zero"),
    ("1 % 0", "division by zero"),
    ("2147483647 + 1", "overflows"),
    ("-2147483647 - 2", "overflows"),
    ("-1 << 1", "negative"),
    ("1 << 32", "shift by 32"),
    ("1 << -1", "shift by -1"),
    ("sizeof(int)", "sizeof"),
    ('"text"', "string"),
    ("1.5", "floating"),
    ("1e3", "floating|integer"),
    ("F(1)", "call"),
    ("UNKNOWN", "unknown"),
    ("1 +", "ends early"),
    ("(1", "ends early"),
    ("1wb", "suffix|integer"),
    ("1 2", "unexpected"),
    ("", "empty"),
    ("(-2147483647 - 1) % -1", "overflows"),
    ("'\u00e9'", "non-ASCII"),
]


@pytest.mark.parametrize(("text", "match"), RAISING, ids=[text or "empty" for text, _ in RAISING])
def test_an_undefined_or_unsupported_expression_raises_value_error(text: str, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        value(text)


def test_the_parser_caps_nesting_by_its_own_message() -> None:
    with pytest.raises(ValueError, match="nests deeper than 64"):
        value("(" * 100 + "1" + ")" * 100)
    assert value("(" * 40 + "1" + ")" * 40) == 1
