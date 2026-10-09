"""C integer constant expressions over macro text, typed as on AAPCS ILP32, for
`Constants.macro`."""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass

__all__ = ["CType", "CValue", "Parsed", "evaluate", "keyword_type", "parse"]


@dataclass(frozen=True, slots=True)
class CType:
    """An integer type: its width, signedness and conversion rank, `int` being 2."""

    bits: int
    signed: bool
    rank: int

    def holds(self, value: int) -> bool:
        """Whether `value` is in this type's range."""
        if self.signed:
            return -(1 << (self.bits - 1)) <= value < 1 << (self.bits - 1)
        return 0 <= value < 1 << self.bits

    def wrapped(self, value: int) -> int:
        """`value` converted to this type modulo its width, as GCC converts."""
        if self.bits == 1:
            return int(value != 0)
        value &= (1 << self.bits) - 1
        if self.signed and value >> (self.bits - 1):
            value -= 1 << self.bits
        return value

    def unsigned(self) -> CType:
        """The unsigned type of the same rank."""
        return CType(self.bits, False, self.rank)


BOOL = CType(1, False, 0)
SCHAR = CType(8, True, 1)
UCHAR = CType(8, False, 1)
SHORT = CType(16, True, 1)
USHORT = CType(16, False, 1)
INT = CType(32, True, 2)
UINT = CType(32, False, 2)
LONG = CType(32, True, 3)
ULONG = CType(32, False, 3)
LLONG = CType(64, True, 4)
ULLONG = CType(64, False, 4)


@dataclass(frozen=True, slots=True)
class CValue:
    """A value and its C type."""

    value: int
    type: CType


type Resolve = Callable[[str], CValue]
"""The value of an identifier, raising ValueError when it has none."""

type TypeName = Callable[[str], CType | None]
"""The integer type a typedef name denotes, None when the name is no such typedef."""

_KEYWORDS = frozenset(("signed", "unsigned", "char", "short", "int", "long", "_Bool", "bool"))
_QUALIFIERS = frozenset(("const", "volatile"))

_TOKEN = re.compile(
    r"""
    (?P<space>\s+)
  | (?P<number>\.?[0-9](?:'?[0-9A-Za-z_.]|[eEpP][+-])*)
  | (?P<char>'(?:[^'\\\n]|\\.)*')
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<name>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<punct><<|>>|<=|>=|==|!=|&&|\|\||[-+*/%~!&^|<>?:()])
    """,
    re.VERBOSE,
)

_BINARY: dict[str, int] = {
    "||": 1,
    "&&": 2,
    "|": 3,
    "^": 4,
    "&": 5,
    "==": 6,
    "!=": 6,
    "<": 7,
    ">": 7,
    "<=": 7,
    ">=": 7,
    "<<": 8,
    ">>": 8,
    "+": 9,
    "-": 9,
    "*": 10,
    "/": 10,
    "%": 10,
}

_ESCAPES = {
    "n": 10,
    "t": 9,
    "r": 13,
    "0": 0,
    "a": 7,
    "b": 8,
    "f": 12,
    "v": 11,
    "\\": 92,
    "'": 39,
    '"': 34,
    "?": 63,
}


def keyword_type(words: list[str]) -> CType | None:
    """The type a set of keyword specifiers names on ILP32, in any order, None when the set is
    no integer type."""
    if not words or any(word not in _KEYWORDS for word in words):
        return None
    signed = words.count("signed")
    unsigned = words.count("unsigned")
    longs = words.count("long")
    base = [word for word in words if word in ("char", "short", "int", "_Bool", "bool")]
    if signed + unsigned > 1 or longs > 2 or len(base) > 1:
        return None
    kind = base[0] if base else "int"
    if kind in ("_Bool", "bool"):
        return BOOL if signed + unsigned + longs == 0 else None
    if kind == "char":
        if longs:
            return None
        return SCHAR if signed else UCHAR
    if kind == "short":
        if longs:
            return None
        return USHORT if unsigned else SHORT
    if longs == 2:
        return ULLONG if unsigned else LLONG
    if longs == 1:
        return ULONG if unsigned else LONG
    return UINT if unsigned else INT


@dataclass(frozen=True, slots=True)
class _Token:
    kind: str
    text: str


def _tokens(text: str) -> list[_Token]:
    tokens: list[_Token] = []
    position = 0
    while position < len(text):
        found = _TOKEN.match(text, position)
        if found is None:
            raise ValueError(f"cannot read {text[position:]!r}")
        kind = found.lastgroup
        assert kind is not None
        if kind == "string":
            raise ValueError("a string literal is no integer")
        if kind != "space":
            tokens.append(_Token(kind, found.group()))
        position = found.end()
    return tokens


_SUFFIXES = frozenset(
    (
        "",
        "u",
        "U",
        "l",
        "L",
        "ll",
        "LL",
        *(u + size for u in "uU" for size in ("l", "L", "ll", "LL")),
        *(size + u for u in "uU" for size in ("l", "L", "ll", "LL")),
    )
)


def _literal_types(text: str, suffix: str, decimal: bool) -> list[CType]:
    if suffix not in _SUFFIXES:
        raise ValueError(f"unsupported integer suffix {suffix!r} in {text!r}")
    size = suffix.lower().replace("u", "")
    ladder = {"": [INT, LONG, LLONG], "l": [LONG, LLONG], "ll": [LLONG]}[size]
    if "u" in suffix.lower():
        return [kind.unsigned() for kind in ladder]
    if decimal:
        return ladder
    return [step for kind in ladder for step in (kind, kind.unsigned())]


def _number(text: str) -> CValue:
    if "." in text:
        raise ValueError(f"{text!r} is a floating constant")
    plain = text.replace("'", "")
    found = re.fullmatch(r"(0[xX][0-9a-fA-F]+|0[bB][01]+|0[0-7]*|[1-9][0-9]*)([uUlL]*)", plain)
    if found is None:
        raise ValueError(f"{text!r} is no integer constant")
    digits, suffix = found.groups()
    if digits[:2] in ("0x", "0X"):
        value, decimal = int(digits[2:], 16), False
    elif digits[:2] in ("0b", "0B"):
        value, decimal = int(digits[2:], 2), False
    elif digits.startswith("0"):
        value, decimal = int(digits, 8), digits == "0"
    else:
        value, decimal = int(digits), True
    for kind in _literal_types(text, suffix, decimal):
        if kind.holds(value):
            return CValue(value, kind)
    raise ValueError(f"{text!r} fits no integer type")


def _character(text: str) -> CValue:
    body = text[1:-1]
    if not body:
        raise ValueError("an empty character constant")
    if body[0] != "\\":
        if len(body) != 1:
            raise ValueError(f"{text!r} is a multi-character constant")
        code = ord(body)
        if code > 0x7F:
            raise ValueError(f"{text!r} is a non-ASCII character constant")
    elif body[1:2] == "x":
        code = int(body[2:], 16)
    elif body[1:2].isdigit():
        code = int(body[1:], 8)
    elif len(body) == 2 and body[1] in _ESCAPES:
        code = _ESCAPES[body[1]]
    else:
        raise ValueError(f"unsupported escape in {text!r}")
    if code > 0xFF:
        raise ValueError(f"{text!r} does not fit a char")
    return CValue(code, INT)


def _promoted(value: CValue) -> CValue:
    if value.type.rank < INT.rank:
        return CValue(value.value, INT)
    return value


def _common(left: CType, right: CType) -> CType:
    if left == right:
        return left
    if left.signed == right.signed:
        return left if left.rank >= right.rank else right
    unsigned, signed = (left, right) if not left.signed else (right, left)
    if unsigned.rank >= signed.rank:
        return unsigned
    if signed.bits > unsigned.bits:
        return signed
    return signed.unsigned()


def _result(value: int, kind: CType, text: str) -> CValue:
    if kind.signed:
        if not kind.holds(value):
            raise ValueError(f"{text} overflows its type")
        return CValue(value, kind)
    return CValue(kind.wrapped(value), kind)


def _truth(value: bool) -> CValue:
    return CValue(int(value), INT)


def _binary(operator: str, left: CValue, right: CValue) -> CValue:
    if operator in ("<<", ">>"):
        shifted = _promoted(left)
        count = _promoted(right).value
        if count < 0 or count >= shifted.type.bits:
            raise ValueError(f"a shift by {count} of a {shifted.type.bits}-bit value")
        if operator == ">>":
            return CValue(shifted.value >> count, shifted.type)
        if shifted.type.signed and shifted.value < 0:
            raise ValueError("a left shift of a negative value")
        return _result(shifted.value << count, shifted.type, "a left shift")
    kind = _common(_promoted(left).type, _promoted(right).type)
    a = kind.wrapped(left.value)
    b = kind.wrapped(right.value)
    match operator:
        case "+":
            return _result(a + b, kind, "an addition")
        case "-":
            return _result(a - b, kind, "a subtraction")
        case "*":
            return _result(a * b, kind, "a multiplication")
        case "/" | "%":
            if b == 0:
                raise ValueError("a division by zero")
            quotient = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
            divided = _result(quotient, kind, "a division")
            if operator == "/":
                return divided
            return _result(a - quotient * b, kind, "a remainder")
        case "<":
            return _truth(a < b)
        case ">":
            return _truth(a > b)
        case "<=":
            return _truth(a <= b)
        case ">=":
            return _truth(a >= b)
        case "==":
            return _truth(a == b)
        case "!=":
            return _truth(a != b)
        case "&":
            return CValue(kind.wrapped(a & b), kind)
        case "^":
            return CValue(kind.wrapped(a ^ b), kind)
        case "|":
            return CValue(kind.wrapped(a | b), kind)
        case _:
            raise ValueError(f"unknown operator {operator}")


@dataclass(frozen=True, slots=True)
class _Value:
    value: CValue


@dataclass(frozen=True, slots=True)
class _Name:
    name: str


@dataclass(frozen=True, slots=True)
class _Cast:
    type: CType
    operand: _Node


@dataclass(frozen=True, slots=True)
class _Unary:
    operator: str
    operand: _Node


@dataclass(frozen=True, slots=True)
class _Binary:
    operator: str
    left: _Node
    right: _Node


@dataclass(frozen=True, slots=True)
class _Conditional:
    condition: _Node
    chosen: _Node
    other: _Node


type _Node = _Value | _Name | _Cast | _Unary | _Binary | _Conditional


_MAX_DEPTH = 64


class _Parser:
    def __init__(self, tokens: list[_Token], type_name: TypeName) -> None:
        self._tokens = tokens
        self._at = 0
        self._type_name = type_name
        self._depth = 0
        self.nodes = 0

    def _nested(self) -> None:
        self._depth += 1
        if self._depth > _MAX_DEPTH:
            raise ValueError(f"the expression nests deeper than {_MAX_DEPTH}")

    def parse(self) -> _Node:
        node = self._conditional()
        if self._at != len(self._tokens):
            raise ValueError(f"unexpected {self._tokens[self._at].text!r}")
        return node

    def _peek(self) -> _Token | None:
        return self._tokens[self._at] if self._at < len(self._tokens) else None

    def _next(self) -> _Token:
        token = self._peek()
        if token is None:
            raise ValueError("the expression ends early")
        self._at += 1
        return token

    def _expect(self, text: str) -> None:
        token = self._next()
        if token.text != text:
            raise ValueError(f"expected {text!r}, found {token.text!r}")

    def _conditional(self) -> _Node:
        condition = self._binary(1)
        token = self._peek()
        if token is None or token.text != "?":
            return condition
        self._at += 1
        chosen = self._conditional()
        self._expect(":")
        other = self._conditional()
        self.nodes += 1
        return _Conditional(condition, chosen, other)

    def _binary(self, lowest: int) -> _Node:
        left = self._unary()
        while True:
            token = self._peek()
            if token is None or token.kind != "punct" or token.text not in _BINARY:
                return left
            level = _BINARY[token.text]
            if level < lowest:
                return left
            self._at += 1
            right = self._binary(level + 1)
            self.nodes += 1
            left = _Binary(token.text, left, right)

    def _cast_type(self) -> CType | None:
        words: list[str] = []
        at = self._at
        while at < len(self._tokens) and self._tokens[at].kind == "name":
            words.append(self._tokens[at].text)
            at += 1
        if at >= len(self._tokens) or self._tokens[at].text != ")" or not words:
            return None
        named = [word for word in words if word not in _QUALIFIERS]
        kind = keyword_type(named)
        if kind is None and len(named) == 1:
            kind = self._type_name(named[0])
        if kind is not None:
            self._at = at + 1
        return kind

    def _unary(self) -> _Node:
        self.nodes += 1
        self._nested()
        try:
            return self._operand()
        finally:
            self._depth -= 1

    def _operand(self) -> _Node:
        token = self._next()
        if token.kind == "punct" and token.text in ("+", "-", "~", "!"):
            return _Unary(token.text, self._unary())
        if token.text == "(":
            kind = self._cast_type()
            if kind is not None:
                return _Cast(kind, self._unary())
            inner = self._conditional()
            self._expect(")")
            return inner
        if token.kind == "number":
            return _Value(_number(token.text))
        if token.kind == "char":
            return _Value(_character(token.text))
        if token.kind == "name":
            if token.text == "sizeof":
                raise ValueError("sizeof is not evaluated")
            follows = self._peek()
            if follows is not None and follows.text == "(":
                raise ValueError(f"{token.text}(...) is a call, not evaluated")
            return _Name(token.text)
        raise ValueError(f"unexpected {token.text!r}")


def _evaluate(node: _Node, resolve: Resolve) -> CValue:
    match node:
        case _Value(value):
            return value
        case _Name(name):
            return resolve(name)
        case _Cast(kind, operand):
            return CValue(kind.wrapped(_evaluate(operand, resolve).value), kind)
        case _Unary("!", operand):
            return _truth(_evaluate(operand, resolve).value == 0)
        case _Unary(operator, operand):
            value = _promoted(_evaluate(operand, resolve))
            if operator == "+":
                return value
            if operator == "-":
                return _result(-value.value, value.type, "a negation")
            return CValue(value.type.wrapped(~value.value), value.type)
        case _Binary("&&", left, right):
            return _truth(
                _evaluate(left, resolve).value != 0 and _evaluate(right, resolve).value != 0
            )
        case _Binary("||", left, right):
            return _truth(
                _evaluate(left, resolve).value != 0 or _evaluate(right, resolve).value != 0
            )
        case _Binary(operator, left, right):
            return _binary(operator, _evaluate(left, resolve), _evaluate(right, resolve))
        case _Conditional(condition, chosen, other):
            taken, skipped = (
                (chosen, other) if _evaluate(condition, resolve).value != 0 else (other, chosen)
            )
            value = _evaluate(taken, resolve)
            kind = _common(_promoted(value).type, _static_type(skipped, resolve, value.type))
            return CValue(kind.wrapped(value.value), kind)


def _static_type(node: _Node, resolve: Resolve, fallback: CType) -> CType:
    """The promoted type of `node` without evaluating it, an identifier's from its value. A
    dead operand of `?:` gives its type only, `fallback` when it has none."""
    match node:
        case _Value(value):
            return _promoted(value).type
        case _Name(name):
            try:
                return _promoted(resolve(name)).type
            except Exception:
                return _promoted(CValue(0, fallback)).type
        case _Cast(kind, _):
            return _promoted(CValue(0, kind)).type
        case _Unary("!", _) | _Binary("&&" | "||" | "<" | ">" | "<=" | ">=" | "==" | "!=", _, _):
            return INT
        case _Unary(_, operand) | _Binary("<<" | ">>", operand, _):
            return _static_type(operand, resolve, fallback)
        case _Binary(_, left, right) | _Conditional(_, left, right):
            return _common(
                _static_type(left, resolve, fallback), _static_type(right, resolve, fallback)
            )


@dataclass(frozen=True, slots=True)
class Parsed:
    """An expression parsed once, evaluated as many times as its identifiers need. `size` is
    its node count, what one evaluation visits at most."""

    _node: _Node
    size: int

    def evaluate(self, resolve: Resolve) -> CValue:
        """The value, each identifier through `resolve`.

        Raises:
            ValueError: An identifier has no value, or the evaluation is undefined in C, or it
                nests too deep.
        """
        try:
            return _evaluate(self._node, resolve)
        except RecursionError:
            raise ValueError("the expression nests too deep") from None


def parse(text: str, type_name: TypeName) -> Parsed:
    """`text` parsed as a C integer constant expression, typedef names through `type_name`.

    Raises:
        ValueError: The text is no such expression.
    """
    tokens = _tokens(text)
    if not tokens:
        raise ValueError("an empty expression")
    parser = _Parser(tokens, type_name)
    return Parsed(parser.parse(), parser.nodes)


def evaluate(text: str, resolve: Resolve, type_name: TypeName) -> CValue:
    """The value of `text` as a C integer constant expression on ILP32.

    Args:
        text: The expression.
        resolve: The value of an identifier, raising ValueError when it has none.
        type_name: The integer type a typedef name denotes, for casts.

    Raises:
        ValueError: The text is no such expression, or its evaluation is undefined in C.
    """
    return parse(text, type_name).evaluate(resolve)
