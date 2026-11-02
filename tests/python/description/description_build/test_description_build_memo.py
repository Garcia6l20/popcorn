import logging
import pickle
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field, fields, replace
from pathlib import Path
from typing import Any, cast

import build_memo
import pytest
from popcorn.stm32.description import _sources, _svd
from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._patches import PatchReport
from popcorn.stm32.description._sources import Sources
from popcorn.stm32.description._svd import SvdDevice

_LOG = logging.getLogger("popcorn")


class _Builds:
    def __init__(self) -> None:
        self.calls = 0

    def __call__(self, sources: Sources) -> tuple[PartDescription, PatchReport]:
        self.calls += 1
        _LOG.warning("%s: built %d", sources.part, self.calls)
        result: Any = ({"patch": sources.patches[0].read_text(), "n": [self.calls]}, [])
        return cast(tuple[PartDescription, PatchReport], result)


def _sources_in(directory: Path, text: str = "a") -> Sources:
    files = {name: directory / name for name in ("svd", "db", "patch")}
    for path in files.values():
        path.write_text(text)
    return Sources(
        part="PART",
        svd=files["svd"],
        mcu_db=files["db"],
        rzone=None,
        cmsis_header=None,
        patches=[files["patch"]],
    )


def _memo() -> tuple[build_memo.BuildMemo, _Builds]:
    builds = _Builds()
    return build_memo.BuildMemo(builds), builds


def test_a_second_call_on_the_same_files_does_not_build(tmp_path: Path) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    first = memo(sources)
    second = memo(sources)
    assert builds.calls == 1
    assert (memo.misses, memo.hits) == (1, 1)
    assert first == second


def test_each_call_owns_its_result(tmp_path: Path) -> None:
    memo, _ = _memo()
    sources = _sources_in(tmp_path)
    first = cast(dict[str, Any], memo(sources)[0])
    first["n"].append(99)
    first["extra"] = 1
    again = cast(dict[str, Any], memo(sources)[0])
    assert again == {"patch": "a", "n": [1]}
    assert again is not first


def test_an_edited_patch_is_built_again(tmp_path: Path) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    before = cast(dict[str, Any], memo(sources)[0])
    sources.patches[0].write_text("b")
    after = cast(dict[str, Any], memo(sources)[0])
    assert builds.calls == 2
    assert (before["patch"], after["patch"]) == ("a", "b")


@pytest.mark.parametrize("name", ["svd", "db"])
def test_an_edited_source_is_built_again(tmp_path: Path, name: str) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    memo(sources)
    (tmp_path / name).write_text("changed")
    memo(sources)
    assert builds.calls == 2


def test_the_same_bytes_at_another_path_are_built_again(tmp_path: Path) -> None:
    memo, builds = _memo()
    (tmp_path / "x").mkdir()
    (tmp_path / "y").mkdir()
    memo(_sources_in(tmp_path / "x"))
    memo(_sources_in(tmp_path / "y"))
    assert builds.calls == 2


def test_a_hit_replays_the_warnings_of_the_build(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    memo, _ = _memo()
    sources = _sources_in(tmp_path)
    memo(sources)
    caplog.clear()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        memo(sources)
    assert [record.getMessage() for record in caplog.records] == ["PART: built 1"]


def test_a_logger_that_drops_warnings_bypasses_the_memo(tmp_path: Path) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    previous = _LOG.level
    _LOG.setLevel(logging.ERROR)
    try:
        memo(sources)
        memo(sources)
    finally:
        _LOG.setLevel(previous)
    assert builds.calls == 2
    assert memo.bypasses == 2


def test_a_patched_description_module_bypasses_the_memo(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    memo(sources)
    monkeypatch.setattr(_sources, "_PATCHES_DIR", tmp_path)
    memo(sources)
    memo(sources)
    assert builds.calls == 3
    monkeypatch.undo()
    memo(sources)
    assert builds.calls == 3


def test_a_patched_path_read_bypasses_the_memo(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    memo(sources)
    original = Path.read_bytes

    def read_bytes(self: Path) -> bytes:
        return original(self)

    monkeypatch.setattr(Path, "read_bytes", read_bytes)
    memo(sources)
    assert builds.calls == 2


def test_the_pickles_kept_stay_under_the_cap(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    memo, builds = _memo()
    monkeypatch.setattr(build_memo, "_MAX_BYTES", 1)
    for index in range(50):
        memo(_sources_in(tmp_path, str(index)))
    assert len(memo.entries) == 1
    assert builds.calls == 50


def test_the_suite_builder_is_the_memo() -> None:
    from popcorn.stm32.description import _builder

    assert isinstance(_builder.build_description_with_report, build_memo.BuildMemo)


def test_two_memos_sharing_a_directory_build_once(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    (tmp_path / "src").mkdir()
    sources = _sources_in(tmp_path / "src")
    builds = _Builds()
    first, second = build_memo.BuildMemo(builds), build_memo.BuildMemo(builds)
    first.share(tmp_path / "shared")
    second.share(tmp_path / "shared")
    first(sources)
    caplog.clear()
    result = cast(dict[str, Any], second(sources)[0])
    assert builds.calls == 1
    assert (first.misses, second.misses, second.loads) == (1, 1, 1)
    assert result == {"patch": "a", "n": [1]}
    assert [record.getMessage() for record in caplog.records] == ["PART: built 1"]
    second(sources)
    assert (second.loads, second.hits) == (1, 1)


def test_a_shared_directory_builds_an_edited_source_again(tmp_path: Path) -> None:
    (tmp_path / "src").mkdir()
    sources = _sources_in(tmp_path / "src")
    builds = _Builds()
    first, second = build_memo.BuildMemo(builds), build_memo.BuildMemo(builds)
    first.share(tmp_path / "shared")
    second.share(tmp_path / "shared")
    first(sources)
    sources.patches[0].write_text("b")
    after = cast(dict[str, Any], second(sources)[0])
    assert builds.calls == 2
    assert after["patch"] == "b"


def test_concurrent_memos_on_one_key_build_it_once(tmp_path: Path) -> None:
    (tmp_path / "src").mkdir()
    sources = _sources_in(tmp_path / "src")
    builds = _Builds()

    def slow(given: Sources) -> tuple[PartDescription, PatchReport]:
        time.sleep(0.2)
        return builds(given)

    memos = [build_memo.BuildMemo(slow) for _ in range(4)]
    for memo in memos:
        memo.share(tmp_path / "shared")
    with ThreadPoolExecutor(4) as pool:
        for future in [pool.submit(memo, sources) for memo in memos]:
            future.result()
    assert builds.calls == 1
    assert sum(memo.loads for memo in memos) == 3


def test_only_the_warnings_of_the_building_thread_are_kept(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    sources = _sources_in(tmp_path)

    def build(given: Sources) -> tuple[PartDescription, PatchReport]:
        other = threading.Thread(target=_LOG.warning, args=("from another thread",))
        other.start()
        other.join()
        return _Builds()(given)

    memo = build_memo.BuildMemo(build)
    memo(sources)
    caplog.clear()
    memo(sources)
    assert [record.getMessage() for record in caplog.records] == ["PART: built 1"]


def test_a_hit_replays_only_the_records_the_logger_still_emits(tmp_path: Path) -> None:
    sources = _sources_in(tmp_path)

    def build(given: Sources) -> tuple[PartDescription, PatchReport]:
        _LOG.info("detail")
        return _Builds()(given)

    seen: list[str] = []

    class Sink(logging.Handler):
        def emit(self, record: logging.LogRecord) -> None:
            seen.append(record.getMessage())

    memo = build_memo.BuildMemo(build)
    previous = _LOG.level
    sink = Sink()
    _LOG.addHandler(sink)
    try:
        _LOG.setLevel(logging.DEBUG)
        memo(sources)
        _LOG.setLevel(logging.WARNING)
        seen.clear()
        memo(sources)
    finally:
        _LOG.removeHandler(sink)
        _LOG.setLevel(previous)
    assert memo.hits == 1
    assert seen == ["PART: built 1"]


@dataclass(frozen=True, slots=True, kw_only=True)
class _WithExtra(Sources):
    extra: Path | None = None
    extras: list[Path] = field(default_factory=list[Path])
    note: str = ""


def test_every_field_of_sources_is_in_the_key(tmp_path: Path) -> None:
    memo, builds = _memo()
    base = _sources_in(tmp_path)
    extra = tmp_path / "extra"
    extra.write_text("1")
    first = _WithExtra(**{f.name: getattr(base, f.name) for f in fields(Sources)}, extra=extra)
    memo(first)
    extra.write_text("2")
    memo(first)
    assert builds.calls == 2
    listed = replace(first, extras=[extra])
    memo(listed)
    extra.write_text("3")
    memo(listed)
    memo(replace(listed, note="other"))
    assert builds.calls == 5


def test_a_field_of_an_unkeyable_type_bypasses_the_memo(tmp_path: Path) -> None:
    memo, builds = _memo()

    @dataclass(frozen=True, slots=True, kw_only=True)
    class Odd(Sources):
        thing: object = None
        other: set[int] = field(default_factory=set[int])

    base = _sources_in(tmp_path)
    odd = Odd(**{f.name: getattr(base, f.name) for f in fields(Sources)})
    memo(odd)
    memo(odd)
    assert (builds.calls, memo.bypasses) == (2, 2)


@pytest.mark.no_build_memo
def test_the_marker_builds_every_call() -> None:
    memo = build_memo.install()
    assert memo.bypassing


def test_without_the_marker_the_memo_is_active() -> None:
    assert not build_memo.install().bypassing


def test_bypassed_builds_each_call_and_keeps_nothing(tmp_path: Path) -> None:
    memo, builds = _memo()
    sources = _sources_in(tmp_path)
    with memo.bypassed():
        memo(sources)
        memo(sources)
    assert (builds.calls, len(memo.entries), memo.bypasses) == (2, 0, 2)
    memo(sources)
    memo(sources)
    assert builds.calls == 3


def test_a_stored_entry_is_smaller_than_its_pickle(tmp_path: Path) -> None:
    sources = _sources_in(tmp_path)

    def build(given: Sources) -> tuple[PartDescription, PatchReport]:
        result: Any = (
            {"rows": [{"name": f"register{n}", "value": n % 7} for n in range(2000)]},
            [],
        )
        return cast(tuple[PartDescription, PatchReport], result)

    memo = build_memo.BuildMemo(build)
    memo.share(tmp_path / "shared")
    first = memo(sources)
    entry = next(iter(memo.entries.values()))
    assert len(entry[0]) < len(pickle.dumps(first)) // 4
    assert memo._size == len(entry[0])  # pyright: ignore[reportPrivateUsage]
    (stored,) = (tmp_path / "shared").glob("*.pkl")
    assert stored.stat().st_size < len(pickle.dumps(first)) // 4
    other = build_memo.BuildMemo(build)
    other.share(tmp_path / "shared")
    assert other(sources) == first
    assert other.loads == 1
    assert other(sources) == first


class _Expansions:
    def __init__(self) -> None:
        self.calls = 0

    def __call__(self, data: bytes, *, file: str) -> SvdDevice:
        self.calls += 1
        if data == b"bad":
            raise ValueError(file)
        result: Any = {"data": data, "file": file, "rows": [self.calls]}
        return cast(SvdDevice, result)


def _expander() -> tuple[build_memo.BuildMemo, _Expansions]:
    expansions = _Expansions()
    return build_memo.BuildMemo(_Builds(), expansions), expansions


def test_the_same_svd_bytes_are_expanded_once() -> None:
    memo, expansions = _expander()
    first = memo.expand_svd(b"svd", file="a.svd")
    second = memo.expand_svd(b"svd", file="a.svd")
    assert expansions.calls == 1
    assert (memo.misses, memo.hits) == (1, 1)
    assert first == second


def test_each_expansion_owns_its_result() -> None:
    memo, _ = _expander()
    memo.expand_svd(b"svd", file="a.svd")
    first = cast(dict[str, Any], memo.expand_svd(b"svd", file="a.svd"))
    first["rows"].append(99)
    again = cast(dict[str, Any], memo.expand_svd(b"svd", file="a.svd"))
    assert again["rows"] == [1]
    assert again is not first


def test_other_svd_bytes_or_another_file_are_expanded_again() -> None:
    memo, expansions = _expander()
    memo.expand_svd(b"svd", file="a.svd")
    memo.expand_svd(b"svd edited", file="a.svd")
    memo.expand_svd(b"svd", file="b.svd")
    assert expansions.calls == 3


def test_a_failed_expansion_is_raised_again_and_keeps_nothing() -> None:
    memo, expansions = _expander()
    for _ in range(2):
        with pytest.raises(ValueError, match=r"a\.svd"):
            memo.expand_svd(b"bad", file="a.svd")
    assert (expansions.calls, len(memo.entries)) == (2, 0)


def test_a_patched_svd_module_bypasses_the_expansion_memo(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    memo, expansions = _expander()
    memo.expand_svd(b"svd", file="a.svd")

    def to_snake(value: str) -> str:
        return value

    monkeypatch.setattr(_svd, "_to_snake", to_snake)
    memo.expand_svd(b"svd", file="a.svd")
    memo.expand_svd(b"svd", file="a.svd")
    assert expansions.calls == 3
    monkeypatch.undo()
    memo.expand_svd(b"svd", file="a.svd")
    assert expansions.calls == 3


def test_bypassed_expands_each_call_and_keeps_nothing() -> None:
    memo, expansions = _expander()
    with memo.bypassed():
        memo.expand_svd(b"svd", file="a.svd")
        memo.expand_svd(b"svd", file="a.svd")
    assert (expansions.calls, len(memo.entries), memo.bypasses) == (2, 0, 2)


def test_memos_sharing_a_directory_expand_an_svd_once(tmp_path: Path) -> None:
    expansions = _Expansions()
    first = build_memo.BuildMemo(_Builds(), expansions)
    second = build_memo.BuildMemo(_Builds(), expansions)
    first.share(tmp_path)
    second.share(tmp_path)
    one = first.expand_svd(b"svd", file="a.svd")
    two = second.expand_svd(b"svd", file="a.svd")
    assert expansions.calls == 1
    assert (second.loads, one) == (1, two)


def test_the_suite_expander_is_the_memo() -> None:
    from popcorn.stm32.description import _builder

    assert isinstance(_builder.build_description_with_report, build_memo.BuildMemo)
    assert _builder.expand_svd.__wrapped__ is _svd.expand_svd  # pyright: ignore


_SVD = b"""<device>
  <name>DEV</name><version>1</version>
  <cpu><name>CM33</name><revision>r0p4</revision><endian>little</endian><mpuPresent>true</mpuPresent>
    <fpuPresent>true</fpuPresent><nvicPrioBits>4</nvicPrioBits><vendorSystickConfig>false</vendorSystickConfig>
  </cpu>
  <peripherals>
    <peripheral><name>TIM1</name><baseAddress>0x40000000</baseAddress>
      <registers>
        <register><name>CR1</name><addressOffset>0</addressOffset><size>32</size><access>read-write</access>
          <fields><field><name>EN</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth></field></fields>
        </register>
      </registers>
    </peripheral>
    <peripheral derivedFrom="TIM1"><name>TIM2</name>
      <baseAddress>0x40000400</baseAddress></peripheral>
  </peripherals>
</device>"""


def test_a_real_expansion_equals_the_unmemoised_one_on_a_miss_and_on_a_hit() -> None:
    expected = _svd.expand_svd(_SVD, file="a.svd")
    assert [p.name for p in expected.peripherals] == ["TIM1", "TIM2"]
    assert expected.peripherals[1].registers
    memo = build_memo.BuildMemo(_Builds(), _svd.expand_svd)
    assert memo.expand_svd(_SVD, file="a.svd") == expected
    assert memo.expand_svd(_SVD, file="a.svd") == expected
    assert (memo.misses, memo.hits) == (1, 1)
