import json
from pathlib import Path
from typing import cast

import black
import jsonschema.validators
from popcorn.stm32.description import _codegen, _model

SCHEMA_PATH = Path(__file__).parents[3] / "data" / "schema" / "part_description.schema.json"
DESCRIPTION_DIR = Path(__file__).parents[3] / "python" / "popcorn" / "stm32" / "description"
MODEL_PATH = DESCRIPTION_DIR / "_model.py"


def _schema() -> dict[str, object]:
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return cast(dict[str, object], data)


def test_generated_model_matches_committed_file() -> None:
    generated = _codegen.generate(_schema())
    formatted = black.format_str(generated, mode=black.Mode(line_length=100))
    assert formatted == MODEL_PATH.read_text(encoding="utf-8")


def test_generated_model_is_already_black_stable() -> None:
    generated = _codegen.generate(_schema())
    formatted = black.format_str(generated, mode=black.Mode(line_length=100))
    assert formatted == generated


def test_enum_values_match_schema_in_order() -> None:
    schema = _schema()
    defs = cast(dict[str, object], schema["$defs"])
    enum_classes = {
        "access": _model.Access,
        "write_effect": _model.WriteEffect,
        "read_effect": _model.ReadEffect,
        "cross_effect": _model.CrossEffect,
        "region_kind": _model.RegionKind,
    }
    for def_name, enum_cls in enum_classes.items():
        node = cast(dict[str, object], defs[def_name])
        expected = cast(list[str], node["enum"])
        actual = [member.value for member in enum_cls]
        assert actual == expected


def test_toy_description_validates_against_schema() -> None:
    toy_path = Path(__file__).parents[3] / "tests" / "data" / "desc" / "toy.json"
    toy = json.loads(toy_path.read_text(encoding="utf-8"))
    schema = _schema()
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)
    validator.validate(toy)
