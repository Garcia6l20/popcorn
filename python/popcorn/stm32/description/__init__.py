"""MCU description: schema-generated types, the JSON binder, source lookup and build cache."""

from popcorn.stm32.description._binder import (
    check_bounds,
    dump_json,
    from_mapping,
    load_json,
    to_mapping,
)
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    CoreDesc,
    CrossEffect,
    CrossRule,
    FieldDesc,
    IrqDesc,
    McuDescription,
    PeripheralDesc,
    ReadEffect,
    RegionAccess,
    RegionDesc,
    RegionKind,
    RegisterDesc,
    WriteEffect,
)
from popcorn.stm32.description._sources import Sources, find_sources, resolve_part

__all__ = [
    "Access",
    "CoreDesc",
    "CrossEffect",
    "CrossRule",
    "DescriptionError",
    "FieldDesc",
    "IrqDesc",
    "McuDescription",
    "PeripheralDesc",
    "ReadEffect",
    "RegionAccess",
    "RegionDesc",
    "RegionKind",
    "RegisterDesc",
    "Sources",
    "WriteEffect",
    "build_description",
    "cached_description",
    "check_bounds",
    "dump_json",
    "find_sources",
    "from_mapping",
    "load_json",
    "resolve_part",
    "to_mapping",
]
