"""Building, loading and resetting a machine."""

from __future__ import annotations

import asyncio
import json
import math
from pathlib import Path

import popcorn
import pytest
from popcorn.units import ms

from .conftest import Machines


def test_machine_builds_loads_and_resets(
    m33: popcorn.PartDescription, basic: popcorn.Image, core_firmware: Path
) -> None:
    m = popcorn.Machine(m33, image=basic)
    m.reset()
    assert m.images == (basic,)
    assert m.images[0].digest == popcorn.Image.elf(core_firmware / "basic.elf").digest
    assert m.description.part == "POPCORN-M33-TEST"


def test_machine_accepts_the_path_of_a_description(basic: popcorn.Image, m33_json: Path) -> None:
    m = popcorn.Machine(m33_json, image=[basic, basic])
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert len(m.images) == 2


def test_image_reports_abi_entry_symbols_and_lines(basic: popcorn.Image) -> None:
    reset = basic.lookup("Reset_Handler")
    assert basic.abi is popcorn.FloatAbi.HARD_DOUBLE
    assert basic.entry == reset.value | 1
    assert reset.function
    assert reset in basic.symbols
    assert basic.segments[0].start == 0x0800_0000
    assert len(basic.digest) == 64
    where = basic.locate(reset.value)
    assert (where.symbol, where.offset) == ("Reset_Handler", 0)
    assert where.file.endswith("/basic.c")
    assert where.line > 0


def test_machine_locates_like_its_image(m33: popcorn.PartDescription, basic: popcorn.Image) -> None:
    m = popcorn.Machine(m33, image=basic)
    address = basic.lookup("Reset_Handler").value + 2
    assert m.locate(address) == basic.locate(address)
    assert m.locate(0x2000_1000) == popcorn.Location(0x2000_1000)


def test_where_locates_the_pc_of_a_stopped_machine(
    m33: popcorn.PartDescription, basic: popcorn.Image
) -> None:
    m = popcorn.Machine(m33, image=basic)
    m.reset()
    stop = asyncio.run(m.run(until="patched", budget=100_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    where = m.where()
    assert (where.symbol, where.offset) == ("patched", 0)
    assert where == m.locate(m.pc) == m.locate(m.reg.pc)
    assert m.last_stop is not None
    assert where == stop.location == m.last_stop.location
    assert asyncio.run(m.step()).reason is popcorn.StopReason.BUDGET
    assert m.where().offset > 0


def test_where_on_an_idle_core_is_its_stop_location(machines: Machines) -> None:
    m = machines("idle")
    stop = asyncio.run(m.run())
    assert stop.reason is popcorn.StopReason.IDLE
    assert m.where() == stop.location
    assert m.where().symbol == "idle_main"
    assert m.where() != m.locate(m.reg.pc)


def test_where_after_a_call_is_the_restored_pc(hooked: popcorn.Machine) -> None:
    before = hooked.where()
    result = asyncio.run(hooked.call("use_delay", budget=1_000_000))
    assert result.stop.reason is popcorn.StopReason.RETURNED
    assert hooked.where() == before
    assert hooked.last_stop is not None
    assert hooked.where() != hooked.last_stop.location


def test_where_inside_a_handler_locates_the_intercepted_function(
    hooked: popcorn.Machine,
) -> None:
    seen: list[popcorn.Location] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        seen.append(hooked.where())
        return c.returns()

    hooked.intercept("hal_delay", handler)
    asyncio.run(hooked.call("use_delay", budget=1_000_000))
    assert [(w.symbol, w.offset) for w in seen] == [("hal_delay", 0)]


def test_where_outside_a_free_run_is_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                m.where()

    asyncio.run(scenario())


def test_bin_image_loads_at_its_address(tmp_path: Path, m33: popcorn.PartDescription) -> None:
    path = tmp_path / "blob.bin"
    path.write_bytes(bytes(range(16)))
    blob = popcorn.Image.bin(path, 0x2000_0100)
    assert blob.segments[0].start == 0x2000_0100
    assert blob.segments[0].file_size == 16
    assert blob.entry is None
    popcorn.Machine(m33, image=blob)


def test_hex_image_loads() -> None:
    image = popcorn.Image.hex(Path(__file__).parents[2] / "native" / "image" / "data" / "image.hex")
    assert image.segments
    assert image.path is not None


def test_a_core_no_arch_module_implements_is_a_config_error(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["core"]["cpu"] = "CM0"
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(popcorn.parse_description(json.dumps(raw), "cm0.json"))
    assert caught.value.key == "core.cpu"
    assert caught.value.expected == "a core an arch module implements, not CM0"


@pytest.mark.parametrize(
    ("options", "key"),
    [
        ({"cpi": 0.0}, "cpi"),
        ({"cpi": math.inf}, "cpi"),
        ({"cpi": math.nan}, "cpi"),
        ({"trace_capacity": 0}, "trace_capacity"),
        ({"core_hz": 1, "cpi": 2.0}, "core_hz"),
        ({"cpi": 5_000_000.0}, "core_hz"),
    ],
)
def test_options_out_of_range_are_config_errors(
    m33: popcorn.PartDescription, options: dict[str, float], key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(
            m33,
            core_hz=int(options.get("core_hz", 0)),
            cpi=options.get("cpi", 1.0),
            trace_capacity=int(options.get("trace_capacity", 4096)),
        )
    assert caught.value.key == key


def test_one_instruction_per_second_is_accepted(m33: popcorn.PartDescription) -> None:
    popcorn.Machine(m33, core_hz=2, cpi=2.0)


def test_description_digest_is_stable(m33: popcorn.PartDescription, m33_json: Path) -> None:
    again = popcorn.load_description(m33_json)
    assert popcorn.description_digest(m33) == popcorn.description_digest(again)
    assert len(popcorn.description_digest(m33)) == 64


def test_description_digest_ignores_generator_and_sources(m33_json: Path, tmp_path: Path) -> None:
    raw = json.loads(m33_json.read_text())
    digests: list[str] = []
    for name, generator, sources in (
        ("a.json", "popcorn 0.1.0 builder 000000000000", {"svd": "0" * 64}),
        ("b.json", "popcorn 9.9.9 builder ffffffffffff", {"svd": "1" * 64, "rzone": "2" * 64}),
    ):
        path = tmp_path / name
        path.write_text(json.dumps(raw | {"generator": generator, "sources": sources}))
        digests.append(popcorn.description_digest(popcorn.load_description(path)))
    assert (
        digests[0] == digests[1] == popcorn.description_digest(popcorn.load_description(m33_json))
    )


def test_import_brings_coverage_and_trace() -> None:
    assert popcorn.coverage.Coverage is not None
    assert popcorn.trace is not None


def test_vector_table_option_stands_in_for_an_executable_region(m33_json: Path) -> None:
    raw = json.loads(m33_json.read_text())
    raw["regions"] = [region for region in raw["regions"] if region["start"] != 0]
    for region in raw["regions"]:
        region["acc"]["execute"] = False
    description = popcorn.parse_description(json.dumps(raw), "no-exec.json")
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(description)
    assert caught.value.key == "regions"
    popcorn.Machine(description, vector_table=0x0800_0000)


def test_a_machine_is_named_after_its_part_by_default(
    m33: popcorn.PartDescription, basic: popcorn.Image
) -> None:
    m = popcorn.Machine(m33, image=basic)
    assert m.name == "POPCORN-M33-TEST"
    assert repr(m) == "Machine(POPCORN-M33-TEST, images=1)"
    named = popcorn.Machine(m33, image=basic, name="node1")
    assert named.name == "node1"
    assert repr(named) == "Machine(node1, POPCORN-M33-TEST, images=1)"
    with pytest.raises(AttributeError):
        object.__setattr__(named, "name", "node2")


@pytest.mark.parametrize("name", ["", "a b", "a\tb", "a.b", "."])
def test_a_name_with_whitespace_or_a_dot_is_refused(
    m33: popcorn.PartDescription, name: str
) -> None:
    with pytest.raises(ValueError, match="name"):
        popcorn.Machine(m33, name=name)


@pytest.mark.parametrize("name", ["", "a b", "a\tb", "a.b", "."])
def test_a_rename_to_a_name_with_whitespace_or_a_dot_is_refused(
    m33: popcorn.PartDescription, name: str
) -> None:
    m = popcorn.Machine(m33, name="node1")
    with pytest.raises(ValueError, match="name"):
        m._rename(name)  # pyright: ignore[reportPrivateUsage]
    assert m.name == "node1"


def fault_message(m33: popcorn.PartDescription, basic: popcorn.Image, name: str | None) -> str:
    m = popcorn.Machine(m33, image=basic, name=name)
    m.reset()
    m.reg.pc = 0x3000_0000
    with pytest.raises(popcorn.FaultError) as caught:
        asyncio.run(m.elapse(ms(1)))
    return str(caught.value)


def test_a_stop_error_of_a_named_machine_names_it(
    m33: popcorn.PartDescription, basic: popcorn.Image
) -> None:
    plain = fault_message(m33, basic, None)
    assert plain.startswith("the run letting time pass stopped with FAULT: ")
    assert fault_message(m33, basic, "POPCORN-M33-TEST") == plain
    assert fault_message(m33, basic, "node1") == f"node1: {plain}"
