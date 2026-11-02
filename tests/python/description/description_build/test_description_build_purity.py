"""Two builds of one source set in one process give the same description and report. The suite
memoises the builder, so this is the case that fails when a build leaks state into the next."""

import build_memo
import pytest
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._sources import Sources, pin_data_sources

from ..conftest import require_pin_data


def _twice(sources: Sources) -> None:
    build = build_memo.install()._build  # pyright: ignore[reportPrivateUsage]
    first, first_report = build(sources)
    second, second_report = build(sources)
    assert dump_json(first) == dump_json(second)
    assert first_report == second_report


def test_two_builds_of_the_u575_sources_agree(u575_sources: Sources) -> None:
    _twice(u575_sources)


def test_two_builds_of_the_h573_sources_agree(h573_sources: Sources) -> None:
    _twice(h573_sources)


def test_two_builds_of_a_pin_data_source_set_agree() -> None:
    try:
        sources = pin_data_sources("STM32H573AIIx", pin_data=require_pin_data(), core=None)
    except MissingSource as exc:
        pytest.skip(f"STM32H573AIIx sources not found: {exc}")
    _twice(sources)
