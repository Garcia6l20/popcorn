"""Dataclasses and enums generated from data/schema/part_description.schema.json.

Regenerate with ``python -m popcorn.stm32.description._codegen``. Do not edit by hand.
"""

from dataclasses import dataclass, field
from enum import StrEnum


class Access(StrEnum):
    READ_ONLY = "read_only"
    WRITE_ONLY = "write_only"
    READ_WRITE = "read_write"
    WRITE_ONCE = "write_once"
    READ_WRITE_ONCE = "read_write_once"


@dataclass(slots=True, kw_only=True)
class CoreDesc:
    cpu: str
    revision: str
    fpu: bool
    fpu_double: bool
    mpu: bool
    mpu_regions: int = field(metadata={"minimum": 0, "maximum": 255})
    mpu_regions_secure: int = field(metadata={"minimum": 0, "maximum": 255})
    sau_regions: int = field(metadata={"minimum": 0, "maximum": 255})
    trustzone: bool
    dsp: bool
    nvic_prio_bits: int = field(metadata={"minimum": 0, "maximum": 255})
    num_irqs: int = field(metadata={"minimum": 0, "maximum": 65535})
    vendor_systick_config: bool
    reset_clock_hz: int = field(metadata={"minimum": 0})


class CrossEffect(StrEnum):
    CLEAR = "clear"
    SET = "set"


@dataclass(slots=True, kw_only=True)
class CrossRule:
    reg: str
    field: str
    target_reg: str
    target_field: str
    effect: CrossEffect


class ReadEffect(StrEnum):
    NONE = "none"
    CLEAR = "clear"
    SET = "set"
    MODIFY = "modify"
    MODIFY_EXTERNAL = "modify_external"


class WriteEffect(StrEnum):
    MODIFY = "modify"
    ONE_TO_CLEAR = "one_to_clear"
    ONE_TO_SET = "one_to_set"
    ONE_TO_TOGGLE = "one_to_toggle"
    ZERO_TO_CLEAR = "zero_to_clear"
    ZERO_TO_SET = "zero_to_set"
    ZERO_TO_TOGGLE = "zero_to_toggle"
    CLEAR = "clear"
    SET = "set"


@dataclass(slots=True, kw_only=True)
class FieldDesc:
    name: str
    lsb: int = field(metadata={"minimum": 0, "maximum": 255})
    width: int = field(metadata={"minimum": 0, "maximum": 255})
    acc: Access
    on_write: WriteEffect
    on_read: ReadEffect


@dataclass(slots=True, kw_only=True)
class IrqDesc:
    number: int = field(metadata={"minimum": 0, "maximum": 65535})
    name: str


@dataclass(slots=True, kw_only=True)
class RegisterDesc:
    name: str
    offset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    width: int = field(metadata={"minimum": 0, "maximum": 255})
    reset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    reset_mask: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    acc: Access
    fields: list[FieldDesc]
    alternate_of: str | None = None


@dataclass(slots=True, kw_only=True)
class PeripheralDesc:
    name: str
    ip: str
    ip_version: str
    base: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    size: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    registers: list[RegisterDesc]
    cross_rules: list[CrossRule]
    irqs: list[int]
    alias_of: str | None = None


@dataclass(slots=True, kw_only=True)
class RegionAccess:
    read: bool
    write: bool
    execute: bool


@dataclass(slots=True, kw_only=True)
class RegionContent:
    offset: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    data: list[int]


class RegionKind(StrEnum):
    ROM = "rom"
    RAM = "ram"


@dataclass(slots=True, kw_only=True)
class RegionDesc:
    name: str
    start: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    size: int = field(metadata={"minimum": 0, "maximum": 4294967295})
    kind: RegionKind
    acc: RegionAccess
    secure: bool
    content: list[RegionContent] | None = None
    controller: str | None = None
    physical: int | None = field(default=None, metadata={"minimum": 0, "maximum": 4294967295})


@dataclass(slots=True, kw_only=True)
class PartDescription:
    schema_version: int = field(default=1, metadata={"const": 1})
    part: str
    generator: str
    sources: dict[str, str]
    core: CoreDesc
    regions: list[RegionDesc]
    peripherals: list[PeripheralDesc]
    irqs: list[IrqDesc]
