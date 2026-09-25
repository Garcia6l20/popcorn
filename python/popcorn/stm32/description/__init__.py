"""MCU description: schema-generated types and the JSON binder."""

from popcorn.stm32.description._binder import (
    check_bounds,
    dump_json,
    from_mapping,
    load_json,
    to_mapping,
)
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
    "WriteEffect",
    "check_bounds",
    "dump_json",
    "from_mapping",
    "load_json",
    "to_mapping",
]
