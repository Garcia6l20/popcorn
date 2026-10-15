import sys
from collections.abc import Callable
from importlib import import_module

get_var: Callable[[str, str], str] = import_module("pcons").get_var

SWITCH_VALUES = {"on": True, "1": True, "off": False, "0": False}


def parse_switch(name: str, raw: object) -> bool:
    """The build switch @p name from @p raw: ON or 1, OFF or 0, any case. A bool is refused."""
    value = None if isinstance(raw, bool) else SWITCH_VALUES.get(str(raw).strip().lower())
    if value is None:
        raise SystemExit(f"popcorn: {name}={raw!r}, expected ON, OFF, 1 or 0")
    return value


def switch(name: str, what: str) -> bool:
    """The build variable @p name as a switch, ON when unset. OFF is announced on stderr."""
    on = parse_switch(name, get_var(name, "ON"))
    if not on:
        print(f"popcorn: {name}=OFF, {what} not built, pass {name}=ON for them", file=sys.stderr)
    return on


build_tests = switch("POPCORN_BUILD_TESTS", "tests and test firmware")
build_examples = switch("POPCORN_BUILD_EXAMPLES", "examples")
