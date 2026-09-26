"""The AAPCS-VFP argument path end to end, on the Cortex-M module's abi firmware."""

from __future__ import annotations

import asyncio

import popcorn
import pytest

from .conftest import ABI_ELF


@pytest.fixture
def abi(m33: popcorn.MachineDescription) -> popcorn.Machine:
    image = popcorn.Image.elf(ABI_ELF)
    assert image.abi is popcorn.FloatAbi.HARD_DOUBLE
    m = popcorn.Machine(m33, image=image)
    m.reset()
    return m


def test_an_intercept_reads_vfp_arguments_and_returns_a_double(abi: popcorn.Machine) -> None:
    seen: list[tuple[int | float, ...]] = []
    floats: list[float] = []

    def mixed(c: popcorn.Call) -> popcorn.Return:
        seen.append(c.args_typed())
        floats.extend((c.arg_f(1), c.arg_f(3)))
        return c.returns(7.5)

    abi.intercept("mixed", mixed)
    stop = asyncio.run(abi.call("call_mixed", budget=100_000)).stop
    assert stop.reason is popcorn.StopReason.USER
    assert seen == [(1, 2.0, 3, 4.0, 5)]
    assert floats == [2.0, 4.0]
    assert abi.var("mixed_seen").value == 7.5


def test_the_abi_override_reads_a_soft_float_function(abi: popcorn.Machine) -> None:
    seen: list[tuple[int | float, ...]] = []

    def soft_mixed(c: popcorn.Call) -> popcorn.Return:
        seen.append(c.args_typed(abi=popcorn.FloatAbi.SOFT))
        return c.returns(7.5, abi=popcorn.FloatAbi.SOFT)

    abi.intercept("soft_mixed", soft_mixed)
    asyncio.run(abi.call("call_soft_mixed", budget=100_000))
    assert seen == [(1, 2.0, 3, 4.0, 5)]
    assert abi.var("mixed_seen").value == 7.5
