"""Checks the locals popcorn.image.dwarf reads from function bodies.

Expected locations and scopes were read with `arm-none-eabi-readelf --debug-dump=info,loc,Ranges`
on the committed image-lto.elf and image-types.elf. data/shared_lists.s builds a unit whose
locals and lexical blocks all share one location list or one range list.
"""

from __future__ import annotations

import gc
import pickle
import shutil
import subprocess
import threading
import time
import warnings
from pathlib import Path

import popcorn.image.dwarf
import pytest
from popcorn.image import DebugInfo, DebugInfoError, Decl, Local, LocationRange, load_debug_info

REPO_ROOT = Path(__file__).resolve().parents[3]
PREBUILT = REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt"
SHARED_LISTS = Path(__file__).resolve().parent / "data" / "shared_lists.s"
MAIN_SOURCE = "tests/firmware/image/lto_main.c"
PEER_SOURCE = "tests/firmware/image/lto_peer.c"
DW_OP_CALL_FRAME_CFA = b"\x9c"


@pytest.fixture(scope="module")
def lto() -> DebugInfo:
    return load_debug_info(PREBUILT / "image-lto.elf")


@pytest.fixture(scope="module")
def types() -> DebugInfo:
    return load_debug_info(PREBUILT / "image-types.elf")


def _locals(info: DebugInfo, function: str) -> dict[str, list[Local]]:
    found: dict[str, list[Local]] = {}
    for local in info.functions[function].locals:
        found.setdefault(local.name, []).append(local)
    return found


def _spans(location: bytes | tuple[LocationRange, ...] | None) -> list[tuple[int, int, bytes]]:
    assert isinstance(location, tuple)
    return [(entry.low, entry.high, entry.expr) for entry in location]


def test_frame_base_of_a_function_with_code(lto: DebugInfo) -> None:
    assert lto.functions["Reset_Handler"].frame_base == DW_OP_CALL_FRAME_CFA


def test_inline_only_function_has_no_locals(lto: DebugInfo) -> None:
    function = lto.functions["lto_bump"]
    assert function.locals == ()
    assert function.frame_base is None


def test_static_local_at_its_address(lto: DebugInfo) -> None:
    (ticks,) = _locals(lto, "Reset_Handler")["lto_ticks"]
    assert ticks.location == b"\x03" + (0x2000001C).to_bytes(4, "little")
    assert ticks.scopes == ()
    assert not ticks.parameter
    assert ticks.inlined is None
    assert ticks.decl == Decl(file=MAIN_SOURCE, line=13)


def test_local_of_a_lexical_block_reached_through_its_origin(lto: DebugInfo) -> None:
    (unused,) = _locals(lto, "Reset_Handler")["lto_unused"]
    assert unused.scopes == ((0x0800001C, 0x08000022), (0x08000022, 0x08000068))
    assert [(low, high) for low, high, _ in _spans(unused.location)] == [
        (0x08000022, 0x08000030),
        (0x08000030, 0x08000034),
        (0x08000034, 0x08000055),
    ]
    assert _spans(unused.location)[0][2] == b"\x03\x1c\x00\x00\x20\x06\x33\x1e\x9f"
    assert unused.inlined is None
    assert unused.decl == Decl(file=MAIN_SOURCE, line=16)


def test_parameter_of_an_inlined_function(lto: DebugInfo) -> None:
    (step,) = _locals(lto, "Reset_Handler")["step"]
    assert step.parameter
    assert step.inlined == "lto_bump"
    assert _spans(step.location) == [(0x0800003C, 0x08000064, b"\x56")]
    assert step.scopes == (
        (0x08000032, 0x08000036),
        (0x0800003C, 0x0800005C),
        (0x0800005E, 0x08000060),
        (0x08000062, 0x08000064),
    )
    assert step.decl == Decl(file=PEER_SOURCE, line=13)


def test_parameter_on_the_frame(types: DebugInfo) -> None:
    (point,) = _locals(types, "sum_point")["p"]
    assert point.parameter
    assert point.location == b"\x91\x78"
    assert point.scopes == ()


def test_location_list_with_a_base_address_entry(types: DebugInfo) -> None:
    (sum_,) = _locals(types, "types_touch")["s"]
    assert _spans(sum_.location) == [
        (0x0800007C, 0x08000082, b"\x50"),
        (0x08000082, 0x080000E2, b"\x55"),
    ]


def test_loop_variable_scoped_to_its_block(types: DebugInfo) -> None:
    (index,) = _locals(types, "sum_variadic")["i"]
    assert index.scopes == (
        (0x08000022, 0x08000024),
        (0x08000030, 0x08000034),
        (0x0800003E, 0x08000048),
    )


def test_location_valid_at_no_pc_is_none(types: DebugInfo) -> None:
    own, inlined = _locals(types, "call_triple")["x"]
    assert own.inlined is None
    assert inlined.inlined == "triple_value"
    assert inlined.location is None
    assert inlined.scopes == ((0x08000058, 0x0800005C),)


@pytest.mark.skipif(shutil.which("arm-none-eabi-as") is None, reason="no arm-none-eabi-as")
@pytest.mark.parametrize(
    ("locals_", "blocks", "entries", "ranges"),
    [(3000, 0, 3000, 1), (0, 3000, 1, 3000)],
    ids=["shared location list", "shared range list"],
)
def test_shared_lists_are_read_once(
    tmp_path: Path, locals_: int, blocks: int, entries: int, ranges: int
) -> None:
    elf = tmp_path / "shared.o"
    symbols = {"LOCALS": locals_, "BLOCKS": blocks, "ENTRIES": entries, "RANGES": ranges}
    defsyms = [arg for name, value in symbols.items() for arg in ("--defsym", f"{name}={value}")]
    subprocess.run(["arm-none-eabi-as", *defsyms, "-o", str(elf), str(SHARED_LISTS)], check=True)
    started = time.perf_counter()
    found = load_debug_info(elf).functions["f"].locals
    elapsed = time.perf_counter() - started
    assert len(found) == 3000
    first = found[0]
    assert isinstance(first.location, tuple)
    assert len(first.location) == entries
    assert len(first.scopes) == (ranges if blocks else 0)
    assert elapsed < 1.0


def _fresh_copy(tmp_path: Path) -> Path:
    copy = tmp_path / "image-lto.elf"
    shutil.copyfile(PREBUILT / "image-lto.elf", copy)
    return copy


def _refuse_locals(monkeypatch: pytest.MonkeyPatch) -> None:
    def refuse(*_: object) -> tuple[Local, ...]:
        raise RuntimeError("locals read")

    monkeypatch.setattr(popcorn.image.dwarf, "_collect_locals", refuse)


def test_load_reads_no_locals(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _refuse_locals(monkeypatch)
    function = load_debug_info(_fresh_copy(tmp_path)).functions["Reset_Handler"]
    assert function.frame_base == DW_OP_CALL_FRAME_CFA
    assert function == function
    assert hash(function) == hash(function)
    assert "locals" not in repr(function)
    assert pickle.loads(pickle.dumps(function)) == function
    with pytest.raises(RuntimeError, match="locals read"):
        _ = function.locals


def test_cached_load_yields_the_same_locals(tmp_path: Path) -> None:
    elf = _fresh_copy(tmp_path)
    first = load_debug_info(elf)
    second = load_debug_info(elf)
    assert second is first
    names = [local.name for local in second.functions["Reset_Handler"].locals]
    assert names == ["lto_unused", "step", "lto_ticks"]
    assert first.functions["Reset_Handler"].locals is second.functions["Reset_Handler"].locals


def test_unpickled_function_reads_its_locals(lto: DebugInfo) -> None:
    function = pickle.loads(pickle.dumps(lto.functions["Reset_Handler"]))
    assert function.locals == lto.functions["Reset_Handler"].locals


def test_locals_read_leaves_no_file_open(tmp_path: Path) -> None:
    info = load_debug_info(_fresh_copy(tmp_path))
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", ResourceWarning)
        assert info.functions["Reset_Handler"].locals
        gc.collect()
    assert [w for w in caught if issubclass(w.category, ResourceWarning)] == []


def test_locals_of_a_changed_file_raise(tmp_path: Path) -> None:
    elf = _fresh_copy(tmp_path)
    function = load_debug_info(elf).functions["Reset_Handler"]
    with elf.open("ab") as stream:
        stream.write(b"\0")
    with pytest.raises(DebugInfoError, match="changed since"):
        _ = function.locals


def test_concurrent_first_reads_share_one_result(tmp_path: Path) -> None:
    function = load_debug_info(_fresh_copy(tmp_path)).functions["Reset_Handler"]
    results: list[tuple[Local, ...]] = []
    start = threading.Barrier(8)

    def read() -> None:
        start.wait()
        results.append(function.locals)

    threads = [threading.Thread(target=read) for _ in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert len(results) == 8
    assert all(result is results[0] for result in results)
