"""Checks C++ method parameters of image-cxx.elf and image-cxx-lto.elf, built from
tests/firmware/image/cxx_methods.cpp without and with -flto.

An out-of-line method's definition holds the named parameters, its in-class declaration
unnamed ones. Under LTO the concrete DIE lists them in another order than the source.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from popcorn.image import Decl, PointerType, StructType, Typedef, load_debug_info

PREBUILT = Path(__file__).resolve().parents[3] / "tests" / "firmware" / "image" / "prebuilt"
SOURCE = "tests/firmware/image/cxx_methods.cpp"


@pytest.mark.parametrize("name", ["image-cxx.elf", "image-cxx-lto.elf"])
def test_method_parameters_named_from_the_definition(name: str) -> None:
    info = load_debug_info(PREBUILT / name)
    add = info.functions["add"]
    assert [(p.name, p.artificial) for p in add.params] == [
        ("this", True),
        ("amount", False),
        ("scale", False),
    ]
    this = info.strip(add.params[0].type)
    assert isinstance(this, PointerType)
    assert this.target is not None
    assert isinstance(info.resolve(this.target), StructType)
    for parameter in add.params[1:]:
        typedef = info.resolve(parameter.type)
        assert isinstance(typedef, Typedef)
        assert typedef.name == "uint32_t"
    assert add.decl == Decl(file=SOURCE, line=13)


@pytest.mark.parametrize("name", ["image-cxx.elf", "image-cxx-lto.elf"])
def test_static_method_has_no_artificial_parameter(name: str) -> None:
    twice = load_debug_info(PREBUILT / name).functions["twice"]
    assert [(p.name, p.artificial) for p in twice.params] == [("value", False)]
    assert twice.decl == Decl(file=SOURCE, line=19)
