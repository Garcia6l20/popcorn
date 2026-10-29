"""The `machine` and `system` decorators: fixtures declared by a function returning a
description."""

from __future__ import annotations

import functools
import inspect
from collections.abc import AsyncIterator, Awaitable, Callable, Iterable, Iterator, Sequence
from typing import cast, overload

import pytest
from _pytest.fixtures import FixtureFunctionDefinition

from popcorn import Machine, MachineDescription, System, SystemDescription
from popcorn.pytest._fixtures import booting, item_of, item_settings, lending, linking
from popcorn.pytest._session import PopcornSession

type Declaring = Callable[..., object]
"""A user function returning a description, or a coroutine function returning one."""

type Ids = Sequence[object | None] | Callable[[object], object | None]
"""What `pytest.fixture(ids=)` takes."""

type Decorator = Callable[[Declaring], FixtureFunctionDefinition]

_OWN = ("request", "_popcorn_session")
_ASSIGNED = ("__module__", "__name__", "__qualname__", "__doc__")


@overload
def machine(fn: Declaring, /) -> FixtureFunctionDefinition: ...


@overload
def machine(
    *,
    config: str | None = None,
    params: Iterable[object] | None = None,
    ids: Ids | None = None,
) -> Decorator: ...


def machine(
    fn: Declaring | None = None,
    /,
    *,
    config: str | None = None,
    params: Iterable[object] | None = None,
    ids: Ids | None = None,
) -> FixtureFunctionDefinition | Decorator:
    """Declare a machine fixture named after `fn`, which returns its `popcorn.MachineDescription`.

    Use it bare, `@popcorn.pytest.machine`, or with arguments,
    `@popcorn.pytest.machine(config="variant")`. `fn` is a fixture function: it may request
    other fixtures, `request` included, and be a coroutine function.

    The fixture gives each test a booted machine named after the fixture, restored from the
    boot snapshot of its key, its config applied. The key boots once per process, shared by
    every fixture of it. Each fixture name has its own machine per key, reused by every test, so
    two fixtures of one description in one test are two machines of one boot. Teardown parks a
    free run the test left started. A machine a test spoiled, by loading another image or
    opening a pty, is replaced by a new one restored from the saved boot. The `popcorn` marker's
    `strict` and `budget` apply.

    Args:
        fn: The function returning the description, when used bare.
        config: The config of the description to apply, None for the base description.
        params: Parameters of the fixture, `request.param` in `fn`, as `pytest.fixture` takes.
        ids: The ids of `params`, as `pytest.fixture` takes them.

    Returns:
        The fixture, or the decorator making it when `fn` is not given.
    """

    def declare(fn: Declaring) -> FixtureFunctionDefinition:
        name = fn.__name__
        _refuse_generator("machine", "MachineDescription", fn)
        call = _caller(fn)
        if inspect.iscoroutinefunction(fn):

            async def booted(**kwargs: object) -> AsyncIterator[Machine]:
                request, session = _own(kwargs)
                description = _checked(
                    name, MachineDescription, await cast(Awaitable[object], call(kwargs))
                )
                test = item_settings(item_of(request), session)
                key = session.key(
                    description, config, test.strict, item_of(request), f"fixture {name!r}"
                )
                with booting(item_of(request), session, key, name):
                    await session.booted_async(key)
                with lending(request, session, test, key, name) as m:
                    yield m

            return _fixture(fn, booted, params, ids)

        def restored(**kwargs: object) -> Iterator[Machine]:
            request, session = _own(kwargs)
            description = _checked(name, MachineDescription, call(kwargs))
            test = item_settings(item_of(request), session)
            key = session.key(
                description, config, test.strict, item_of(request), f"fixture {name!r}"
            )
            with lending(request, session, test, key, name) as m:
                yield m

        return _fixture(fn, restored, params, ids)

    return declare if fn is None else declare(fn)


@overload
def system(fn: Declaring, /) -> FixtureFunctionDefinition: ...


@overload
def system(*, params: Iterable[object] | None = None, ids: Ids | None = None) -> Decorator: ...


def system(
    fn: Declaring | None = None,
    /,
    *,
    params: Iterable[object] | None = None,
    ids: Ids | None = None,
) -> FixtureFunctionDefinition | Decorator:
    """Declare a linked system fixture named after `fn`, which returns its
    `popcorn.SystemDescription`.

    Use it bare, `@popcorn.pytest.system`, or with arguments. `fn` is a fixture function, as for
    `machine`. The fixture gives each test the description booted and linked, a
    `popcorn.System`. Each member is a new machine named as the member, restored from the boot
    snapshot of its own description's key, booted once per key and process: two members of one
    description are two machines of one boot. Each stands at its boot stop, its system time from
    `SystemDescription.offsets`. The `popcorn` marker's `strict` and `budget` apply to every
    member, `budget` as its `default_budget`. A failing test reports each member as
    `<fixture> member <name>`, and `--popcorn-trace-on-fail` keeps
    `<test>.<fixture>.<member>.pctr`. Teardown halts a free run the test left started, closes
    the system, then parks and closes the members.

    Args:
        fn: The function returning the description, when used bare.
        params: Parameters of the fixture, `request.param` in `fn`, as `pytest.fixture` takes.
        ids: The ids of `params`, as `pytest.fixture` takes them.

    Returns:
        The fixture, or the decorator making it when `fn` is not given.
    """

    def declare(fn: Declaring) -> FixtureFunctionDefinition:
        name = fn.__name__
        _refuse_generator("system", "SystemDescription", fn)
        call = _caller(fn)
        awaited = inspect.iscoroutinefunction(fn)

        async def linked(**kwargs: object) -> AsyncIterator[System]:
            request, session = _own(kwargs)
            value = call(kwargs)
            if awaited:
                value = await cast(Awaitable[object], value)
            spec = _checked(name, SystemDescription, value)
            async with linking(request, session, spec, name) as found:
                yield found

        return _fixture(fn, linked, params, ids)

    return declare if fn is None else declare(fn)


def _refuse_generator(decorator: str, kind: str, fn: Declaring) -> None:
    if inspect.isgeneratorfunction(fn) or inspect.isasyncgenfunction(fn):
        raise TypeError(
            f"@popcorn.pytest.{decorator} takes a function returning a popcorn.{kind}, "
            f"not a generator, return the description instead of yielding it: {fn.__name__}"
        )


def _caller(fn: Declaring) -> Callable[[dict[str, object]], object]:
    names = [
        p.name
        for p in inspect.signature(fn).parameters.values()
        if p.kind in (inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.KEYWORD_ONLY)
    ]

    def call(kwargs: dict[str, object]) -> object:
        return fn(**{name: kwargs[name] for name in names if name in kwargs})

    return call


def _fixture(
    fn: Declaring,
    wrapper: Callable[..., object],
    params: Iterable[object] | None,
    ids: Ids | None,
) -> FixtureFunctionDefinition:
    user = inspect.signature(fn)
    parameters = list(user.parameters.values())
    rest = [p for p in parameters if p.kind is inspect.Parameter.VAR_KEYWORD]
    own = [
        inspect.Parameter(name, inspect.Parameter.KEYWORD_ONLY)
        for name in _OWN
        if name not in user.parameters
    ]
    head = [p for p in parameters if p.kind is not inspect.Parameter.VAR_KEYWORD]
    functools.update_wrapper(wrapper, fn, _ASSIGNED, ())
    vars(wrapper).pop("__wrapped__")
    vars(wrapper)["__signature__"] = user.replace(
        parameters=[*head, *own, *rest], return_annotation=inspect.Signature.empty
    )
    return pytest.fixture(name=fn.__name__, params=params, ids=ids)(wrapper)


def _own(kwargs: dict[str, object]) -> tuple[pytest.FixtureRequest, PopcornSession]:
    request = kwargs["request"]
    session = kwargs["_popcorn_session"]
    assert isinstance(request, pytest.FixtureRequest)
    assert isinstance(session, PopcornSession)
    return request, session


def _checked[T](name: str, kind: type[T], value: object) -> T:
    if not isinstance(value, kind):
        pytest.fail(
            f"the popcorn fixture {name} returned {type(value).__name__}, not a "
            f"popcorn.{kind.__name__}",
            pytrace=False,
        )
    return value
