import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

import pytest
from popcorn.stm32.description._binder import (
    check_bounds,
    dump_json,
    from_mapping,
    load_json,
    to_mapping,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    McuDescription,
    PeripheralDesc,
    RegionDesc,
    RegisterDesc,
)

TOY_PATH = Path(__file__).parents[3] / "tests" / "data" / "desc" / "toy.json"


def _toy() -> dict[str, object]:
    data = json.loads(TOY_PATH.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return cast(dict[str, object], data)


def test_round_trip_toy_description() -> None:
    description = load_json(TOY_PATH.read_text(encoding="utf-8"), file=str(TOY_PATH))
    dumped = json.loads(dump_json(description))
    assert dumped == _toy()


def test_register_alternate_of_round_trips() -> None:
    reg = from_mapping(
        RegisterDesc,
        {
            "name": "ISR_ALT",
            "offset": 0,
            "width": 32,
            "reset": 0,
            "reset_mask": 0xFFFFFFFF,
            "acc": "read_only",
            "fields": [],
            "alternate_of": "ISR",
        },
        file="test",
    )
    assert reg.alternate_of == "ISR"
    mapping = cast(dict[str, object], to_mapping(reg))
    assert mapping["alternate_of"] == "ISR"


def test_region_physical_zero_is_kept_not_omitted() -> None:
    region = from_mapping(
        RegionDesc,
        {
            "name": "ALIAS",
            "start": 0x1000_0000,
            "size": 0x1000,
            "kind": "ram",
            "acc": {"read": True, "write": True, "execute": False},
            "secure": False,
            "physical": 0,
        },
        file="test",
    )
    assert region.physical == 0
    mapping = cast(dict[str, object], to_mapping(region))
    assert mapping["physical"] == 0


def test_empty_peripherals_and_sources() -> None:
    data = _toy()
    data["peripherals"] = []
    data["irqs"] = []
    description = from_mapping(McuDescription, data, file="test")
    assert description.peripherals == []
    assert description.irqs == []


def test_unknown_key_reports_nested_path() -> None:
    data = _toy()
    peripherals = cast(list[dict[str, object]], data["peripherals"])
    peripherals[0]["bogus"] = 1
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "peripherals[0].bogus"


def test_missing_required_key() -> None:
    data = _toy()
    del data["part"]
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "part"


def test_null_for_optional_is_refused() -> None:
    data = _toy()
    peripherals = cast(list[dict[str, object]], data["peripherals"])
    peripherals[0]["alias_of"] = None
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "peripherals[0].alias_of"


def test_true_for_integer_is_refused() -> None:
    data = _toy()
    core = cast(dict[str, object], data["core"])
    core["num_irqs"] = True
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "core.num_irqs"


def test_out_of_range_uint8_is_refused() -> None:
    data = _toy()
    core = cast(dict[str, object], data["core"])
    core["nvic_prio_bits"] = 256
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "core.nvic_prio_bits"


def test_negative_integer_is_refused() -> None:
    data = _toy()
    core = cast(dict[str, object], data["core"])
    core["nvic_prio_bits"] = -1
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "core.nvic_prio_bits"


def test_wrong_enum_string_is_refused() -> None:
    data = _toy()
    peripherals = cast(list[dict[str, object]], data["peripherals"])
    registers = cast(list[dict[str, object]], peripherals[0]["registers"])
    registers[0]["acc"] = "bogus_access"
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "peripherals[0].registers[0].acc"


def test_string_for_list_is_refused() -> None:
    data = _toy()
    data["peripherals"] = "not-a-list"
    with pytest.raises(DescriptionError) as excinfo:
        from_mapping(McuDescription, data, file="test")
    assert excinfo.value.key == "peripherals"


def test_check_bounds_accepts_valid_description() -> None:
    description = from_mapping(McuDescription, _toy(), file="test")
    check_bounds(description, file="test")


@dataclass(slots=True, kw_only=True)
class _Bounded:
    value: int = field(metadata={"minimum": 0, "maximum": 10})


def test_check_bounds_rejects_a_directly_built_out_of_range_object() -> None:
    obj = _Bounded(value=11)
    with pytest.raises(DescriptionError) as excinfo:
        check_bounds(obj, file="test")
    assert excinfo.value.key == "value"


@dataclass(slots=True, kw_only=True)
class _NestedContainer:
    string_lists: dict[str, list[str]]
    int_lists: dict[str, list[int]]
    peripherals: list[PeripheralDesc]


def test_dict_of_string_lists_binds() -> None:
    result = from_mapping(
        _NestedContainer,
        {
            "string_lists": {"renames": ["OLD", "NEW"]},
            "int_lists": {"drop": [97, 108]},
            "peripherals": [],
        },
        file="test",
    )
    assert result.string_lists == {"renames": ["OLD", "NEW"]}
    assert result.int_lists == {"drop": [97, 108]}


def test_list_of_dataclasses_binds() -> None:
    peripherals = cast(list[dict[str, object]], _toy()["peripherals"])
    result = from_mapping(
        _NestedContainer,
        {"string_lists": {}, "int_lists": {}, "peripherals": [peripherals[0]]},
        file="test",
    )
    assert len(result.peripherals) == 1
    assert result.peripherals[0].name == "UART1"


@dataclass(slots=True, kw_only=True)
class _Selector:
    ip: str | None = field(default=None, metadata={"selector_group": "target"})
    instances: list[str] | None = field(default=None, metadata={"selector_group": "target"})


def test_selector_with_exactly_one_field_set() -> None:
    result = from_mapping(_Selector, {"ip": "usart"}, file="test")
    assert result.ip == "usart"
    assert result.instances is None


def test_selector_with_both_fields_set_is_refused() -> None:
    with pytest.raises(DescriptionError):
        from_mapping(_Selector, {"ip": "usart", "instances": ["UART1"]}, file="test")


def test_selector_with_neither_field_set_is_refused() -> None:
    with pytest.raises(DescriptionError):
        from_mapping(_Selector, {}, file="test")


@dataclass(slots=True, kw_only=True)
class _AllOptionalNotASelector:
    a: str | None = None
    b: str | None = None


def test_all_optional_dataclass_without_metadata_is_not_a_selector() -> None:
    result = from_mapping(_AllOptionalNotASelector, {}, file="test")
    assert result.a is None
    assert result.b is None


def test_region_controller_round_trips() -> None:
    data = _toy()
    regions = cast(list[dict[str, object]], data["regions"])
    regions[0]["controller"] = "UART1"
    description = load_json(json.dumps(data), file="controller.json")
    assert description.regions[0].controller == "UART1"
    assert json.loads(dump_json(description)) == data
