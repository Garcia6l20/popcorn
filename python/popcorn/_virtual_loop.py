"""The virtual-time asyncio loop of a machine: `time()` is board time, timers are machine events,
and nothing blocks or reads a wall clock."""

from __future__ import annotations

import asyncio
import collections
import contextvars
import heapq
import itertools
import logging
import sys
import threading
from collections.abc import Callable, Coroutine
from typing import Any, NoReturn, cast

from popcorn import _core
from popcorn._errors import ModelError

MAX_CALLBACKS_PER_STAMP = 65536

_MAX_ARM_PS = 2**64 - 1
_PS_PER_SECOND = 10**12
_NEVER_PS = 1 << 256
_COMPACT_FROM = 100
_ASYNCGEN_SWEEP_FROM = 256

_events: Any = asyncio.events
_tasks: Any = asyncio.tasks
_logger = logging.getLogger("asyncio")
_get_ident = threading.get_ident


class VirtualLoopUnsupported(NotImplementedError):
    """A `VirtualLoop` has no such call: it never blocks and has no I/O, executor, signal or wall
    clock, and it runs only when its machine runs."""


def _seconds_to_ps(value: object, what: str) -> int:
    """`value` seconds as the nearest whole picosecond, exact for any finite number."""
    ratio = getattr(value, "as_integer_ratio", None)
    if ratio is None:
        raise TypeError(f"{what} takes a number of seconds, not {value!r}")
    try:
        numerator, denominator = cast(Callable[[], tuple[int, int]], ratio)()
    except OverflowError:
        return _NEVER_PS if cast(float, value) > 0 else -_NEVER_PS
    return (2 * numerator * _PS_PER_SECOND + denominator) // (2 * denominator)


def _first_iteration(agen: Any) -> None:
    loop = _events._get_running_loop()
    if isinstance(loop, VirtualLoop):
        loop._note_asyncgen(agen)  # pyright: ignore[reportPrivateUsage]


async def _close_asyncgen(agen: Any) -> None:
    await agen.aclose()


class _Timer(asyncio.TimerHandle):
    """A timer whose cancel from outside a step withdraws the machine event it armed."""

    __slots__ = ()

    def cancel(self) -> None:
        if self.cancelled():
            return
        loop = cast(VirtualLoop, self._loop)  # pyright: ignore[reportAttributeAccessIssue]
        loop._check_cancel()  # pyright: ignore[reportPrivateUsage]
        super().cancel()
        loop._timer_cancelled()  # pyright: ignore[reportPrivateUsage]


class VirtualLoop(asyncio.AbstractEventLoop):
    """A real `asyncio.AbstractEventLoop` for a stack that lives on the machine's thread, stepped by
    machine events. Get it from `Machine.loop` or `UartDevice.loop`. One per `Machine`, a member of
    a `System` has its own. It stays alongside the real loop the test runs on.

    Step: an armed timer is one event in the machine's queue. When it fires, and when a device hook
    runs, the loop runs that hook, every ready callback and every timer due until none is left, at
    most 65536 callbacks per stamp, then arms the earliest timer. A callback may `create_task`, and
    the task's first step happens at the same stamp.

    Time: `time()` is logical board time in seconds, inside a step the due time of the timer or the
    `at` of the device hook, monotonic through every reset kind. The machine's clock moves by block,
    so a step runs at the first block boundary at or after its stamp and `Machine.time` can read up
    to one block later. A stamp is an integer of picoseconds, a delay rounds to the nearest one, and
    a positive delay is at least 1 ps. Outside a step `time()` is the machine's board time.

    Resets: the loop and its timers go on through every reset kind, each timer at its instant.
    `UartDevice.reset` is told, and a device cancels what it wants there. A snapshot holds nothing
    of the loop. A restore leaves its timers and tasks in place, and stamps count from the restored
    clock.

    Threads: callbacks and tasks run on the machine's thread. A parked machine takes `call_soon`,
    `call_later`, `call_at`, `create_task` and a timer's `cancel` from the test thread, and they run
    when it next runs. While it runs those raise `MachineRunning` off its thread, as `Machine.mem`
    does from a callback. I/O, executors, signals, `call_soon_threadsafe` and the `run_*` methods
    raise `VirtualLoopUnsupported`.

    Errors: an exception from a callback or a device hook, a task failure nothing retrieved by the
    end of the step, and the callback bound stop the run as a `ModelError` from its goal, naming the
    device, the task or the callback. Every callback of the step finishes first, and the machine
    stays parked at that boundary and usable. `set_exception_handler` takes over everything but the
    bound. A task failure is told to the handler at the end of its step too, so a run stays the same
    whatever a collection does. An awaiter already linked to a failed task wakes inside the step, so
    it retrieves the failure in time. A retrieval made only by another step counts as too late, also
    at the same exact picosecond, such as the frame of a second USART.

    A repeating timer counts as something due: a bare `Machine.run()` with one armed runs on, so
    give it `for_` or `deadline`.

    Close: `close()` cancels the tasks and runs them to their `finally`, closes each async generator
    left unfinished, and drops the rest. The loop holds one entry per generator abandoned unfinished
    until then. Call it before dropping a machine whose stack has an `await` in a `finally`: a
    dropped machine is collected with its loop and tasks, whose finalizers run in the order the
    collector picks, so a coroutine may be finalized first and an `await` in its `finally` fails,
    as an unraisable `RuntimeError` at that collection. The `popcorn.pytest` fixtures close the loop
    of every machine they drop. The machine's finalizer closes the loop too, for what is left. The
    loop keeps no machine alive.
    """

    def __init__(self, name: str) -> None:
        """Make a loop with no port yet, for `_bind`."""
        self._name = name
        self._port: _core.VirtualLoopPort | None = None
        self._ready: collections.deque[asyncio.Handle] = collections.deque()
        self._timers: list[tuple[int, int, _Timer]] = []
        self._order = itertools.count()
        self._cancelled_timers = 0
        self._tasks: dict[asyncio.Task[Any], None] = {}
        self._failed: list[asyncio.Task[Any]] = []
        self._errors: list[dict[str, Any]] = []
        self._overrun: ModelError | None = None
        self._asyncgens: dict[Any, None] = {}
        self._asyncgen_sweep = _ASYNCGEN_SWEEP_FROM
        self._hooks_thread: int | None = None
        self._now_ps = 0
        self._base_ps = 0
        self._armed: int | None = None
        self._stepping: int | None = None
        self._stamp_counted = -1
        self._callbacks = 0
        self._closed = False
        self._closing = False
        self._close_pending = False
        self._debug = False
        self._task_factory: Any = None
        self._exception_handler: Any = None

    def __repr__(self) -> str:
        return f"<VirtualLoop of {self._name}>"

    def _bind(self, port: _core.VirtualLoopPort) -> None:
        """Set what the loop arms its machine event through."""
        self._port = port

    def time(self) -> float:
        """Logical board time in seconds, see the class doc.

        Raises:
            MachineRunning: Called off the machine's thread while it runs.
        """
        return self._time_ps() / 1e12

    def is_running(self) -> bool:
        """Whether a step is running."""
        return self._stepping is not None

    def is_closed(self) -> bool:
        """Whether the loop was closed."""
        return self._closed

    def get_debug(self) -> bool:
        """The debug flag of `set_debug`."""
        return self._debug

    def set_debug(self, enabled: bool) -> None:
        """Set asyncio's debug flag, which adds source tracebacks to handles and tasks."""
        self._debug = enabled

    def create_future(self) -> asyncio.Future[Any]:
        """A future of this loop."""
        return asyncio.Future(loop=self)

    def create_task(self, coro: Any, **kwargs: Any) -> asyncio.Task[Any]:
        """Schedule `coro` as a task of this loop, through the task factory if one is set. The loop
        holds the task until it ends.

        Raises:
            RuntimeError: The loop is closed.
            MachineRunning: Called off the machine's thread while it runs.
        """
        self._check_open()
        if self._stepping != _get_ident():
            self._time_ps()
        if self._task_factory is None:
            task: asyncio.Task[Any] = asyncio.Task(coro, loop=self, **kwargs)
        else:
            task = self._task_factory(self, coro, **kwargs)
        cast(Any, task)._log_destroy_pending = False
        self._tasks[task] = None
        task.add_done_callback(self._task_done)
        return task

    def set_task_factory(self, factory: Any) -> None:
        """Set what `create_task` calls, or None for a plain `asyncio.Task`.

        Raises:
            TypeError: `factory` is neither callable nor None.
        """
        if factory is not None and not callable(factory):
            raise TypeError(f"a task factory must be callable or None, not {factory!r}")
        self._task_factory = factory

    def get_task_factory(self) -> Any:
        """What `set_task_factory` set."""
        return self._task_factory

    def call_soon(
        self, callback: Any, *args: Any, context: contextvars.Context | None = None
    ) -> asyncio.Handle:
        """Run `callback` at the current stamp, or at the machine's next dispatch from outside a
        step.

        Raises:
            TypeError: `callback` is not callable.
            RuntimeError: The loop is closed.
            MachineRunning: Called off the machine's thread while it runs.
        """
        self._check_callable(callback)
        self._check_open()
        if self._stepping == _get_ident():
            handle = asyncio.Handle(callback, args, self, context)
            self._ready.append(handle)
            return handle
        now = self._time_ps()
        handle = asyncio.Handle(callback, args, self, context)
        self._ready.append(handle)
        self._arm(now)
        return handle

    def call_later(
        self,
        delay: float,
        callback: Any,
        *args: Any,
        context: contextvars.Context | None = None,
    ) -> asyncio.TimerHandle:
        """Run `callback` `delay` seconds after `time()`, rounded to the nearest picosecond, at
        least 1 ps for a positive delay.

        Raises:
            TypeError: `delay` is no number, or `callback` is not callable.
            RuntimeError: The loop is closed.
            MachineRunning: Called off the machine's thread while it runs.
        """
        ticks = _seconds_to_ps(delay, "call_later()")
        self._check_callable(callback)
        self._check_open()
        if ticks <= 0:
            ticks = 1 if delay > 0 else 0
        return self._timer(self._time_ps() + ticks, callback, args, context)

    def call_at(
        self,
        when: float,
        callback: Any,
        *args: Any,
        context: contextvars.Context | None = None,
    ) -> asyncio.TimerHandle:
        """Run `callback` at `when` on `time()`'s scale, or at the current stamp when that passed.

        Raises:
            TypeError: `when` is no number, or `callback` is not callable.
            RuntimeError: The loop is closed.
            MachineRunning: Called off the machine's thread while it runs.
        """
        due = _seconds_to_ps(when, "call_at()")
        self._check_callable(callback)
        self._check_open()
        return self._timer(max(due, self._time_ps()), callback, args, context)

    def get_exception_handler(self) -> Any:
        """What `set_exception_handler` set."""
        return self._exception_handler

    def set_exception_handler(self, handler: Any) -> None:
        """Set what hears a callback's exception, a device hook's and an unretrieved task failure,
        called as `handler(loop, context)` at the end of the step or at the callback's return.
        With a handler they stop no run, unless it calls `default_exception_handler`.

        Raises:
            TypeError: `handler` is neither callable nor None.
        """
        if handler is not None and not callable(handler):
            raise TypeError(f"an exception handler must be callable or None, not {handler!r}")
        self._exception_handler = handler

    def default_exception_handler(self, context: dict[str, Any]) -> None:
        """Keep `context` to raise as a `ModelError` at the end of the step."""
        self._errors.append(context)

    def call_exception_handler(self, context: dict[str, Any]) -> None:
        """Tell the exception handler, or the default one, what `context` holds. A report from a
        future or task finalizer comes at a time a collection picks, so it goes to the `asyncio`
        logger instead and the run stays the same."""
        message = str(context.get("message", ""))
        exception = context.get("exception")
        if self._closing and "cannot reuse already awaited" in str(exception):
            return
        if self._closing or message.endswith(("never retrieved", "destroyed but it is pending!")):
            _logger.error("%s: %s", self._name, message, exc_info=exception)
            return
        handler = self._exception_handler
        if handler is None:
            self.default_exception_handler(context)
            return
        try:
            handler(self, context)
        except Exception as error:
            self.default_exception_handler(
                {"message": f"the exception handler raised {error!r}", "exception": error}
            )

    def close(self) -> None:
        """Cancel every task and run it to its `finally`, close the async generators left
        unfinished, and drop the rest. Later calls do nothing. A task still pending after its
        cancel is dropped too, and what these raise goes to the `asyncio` logger. From a callback
        it takes effect at the end of its step.

        Raises:
            MachineRunning: Called off the machine's thread while it runs.
        """
        if self._closed:
            return
        if self._stepping != _get_ident():
            self._time_ps()
        self._finalize()

    def _finalize(self) -> None:
        if self._closed:
            return
        if self._stepping is not None:
            self._close_pending = True
            return
        self._close_now()

    def _close_now(self) -> None:
        self._closing = True
        self._callbacks = 0
        pending = list(self._tasks)
        known = set(pending)
        others = [task for task in asyncio.all_tasks(self) if task not in known]
        pending += sorted(others, key=lambda task: task.get_name())
        entered = self._enter()
        try:
            for task in pending:
                task.cancel()
            self._drain()
            for agen in list(self._asyncgens):
                self.create_task(_close_asyncgen(agen))
            self._drain()
        finally:
            self._leave(entered)
        for task in pending:
            cast(Any, task)._log_destroy_pending = False
        self._settle_failed()
        self._errors.clear()
        self._overrun = None
        self._tasks.clear()
        self._asyncgens.clear()
        self._ready.clear()
        self._timers.clear()
        self._cancelled_timers = 0
        self._closed = True
        self._armed = None
        port = self._port
        if port is not None:
            try:
                port.disarm()
            except Exception:
                _logger.debug("%s: no machine to withdraw the event from", self._name)

    def _check_open(self) -> None:
        if self._closed:
            raise RuntimeError("Event loop is closed")

    @staticmethod
    def _check_callable(callback: object) -> None:
        if not callable(callback):
            raise TypeError(f"a callback must be callable, not {callback!r}")

    def _time_ps(self) -> int:
        if self._closed or self._stepping == _get_ident():
            return self._now_ps
        port = self._port
        assert port is not None
        return max(self._now_ps, port.board_time_ps())

    def _timer(
        self,
        due: int,
        callback: Any,
        args: tuple[Any, ...],
        context: contextvars.Context | None,
    ) -> asyncio.TimerHandle:
        handle = _Timer(due / 1e12, callback, args, self, context)
        cast(Any, handle)._scheduled = True
        heapq.heappush(self._timers, (due, next(self._order), handle))
        if self._stepping != _get_ident():
            self._arm(self._time_ps())
        return handle

    def _timer_handle_cancelled(self, handle: asyncio.TimerHandle) -> None:
        if cast(Any, handle)._scheduled:
            self._cancelled_timers += 1

    def _check_cancel(self) -> None:
        if not self._closed and self._stepping != _get_ident():
            self._time_ps()

    def _timer_cancelled(self) -> None:
        if not self._closed and self._stepping != _get_ident():
            self._arm(self._time_ps())

    def _arm(self, now: int) -> None:
        timers = self._timers
        if self._cancelled_timers > _COMPACT_FROM and self._cancelled_timers * 2 > len(timers):
            kept = [entry for entry in timers if not entry[2].cancelled()]
            for _, _, handle in timers:
                cast(Any, handle)._scheduled = not handle.cancelled()
            heapq.heapify(kept)
            self._timers = timers = kept
            self._cancelled_timers = 0
        while timers and timers[0][2].cancelled():
            cast(Any, heapq.heappop(timers)[2])._scheduled = False
            self._cancelled_timers -= 1
        due: int | None
        if self._ready:
            if self._armed is not None and self._armed <= now:
                return
            due = now
        elif timers and timers[0][0] <= _MAX_ARM_PS:
            due = timers[0][0]
        else:
            due = None
        if due == self._armed:
            return
        port = self._port
        assert port is not None
        if due is None:
            port.disarm()
        else:
            port.arm(due)
        self._armed = due

    def _fire(self, stamp: int) -> None:
        self._armed = None
        self._step(stamp, None, ())

    def _run_hook(self, at: int, hook: Callable[..., None], *args: Any) -> None:
        """Run a device hook at `at`, in `Machine.time`'s base, inside a step, and the step on to
        its end."""
        port = self._port
        assert port is not None
        self._step(port.board_base_ps() + at, hook, args)

    def _run_alone(self, at: int, hook: Callable[..., None], *args: Any) -> None:
        """Run a device hook at `at` as `_run_hook` does, on whichever thread calls, and leave what
        it made ready for the machine's next dispatch. A reset from a `finally` at `close()` gets
        here inside a step, which goes on."""
        port = self._port
        assert port is not None
        self._base_ps = port.board_base_ps()
        stamp = self._base_ps + at
        if self._stepping == _get_ident():
            if stamp > self._now_ps:
                self._now_ps = stamp
            self._call_hook(hook, args)
            return
        entered = self._enter()
        try:
            if stamp > self._now_ps:
                self._now_ps = stamp
            self._call_hook(hook, args)
        finally:
            self._leave(entered)
        self._finish()

    def _step(self, stamp: int, hook: Callable[..., None] | None, args: tuple[Any, ...]) -> None:
        port = self._port
        assert port is not None
        self._base_ps = port.board_base_ps()
        if stamp > self._now_ps:
            self._now_ps = stamp
        if self._now_ps != self._stamp_counted:
            self._stamp_counted = self._now_ps
            self._callbacks = 0
        entered = self._enter()
        try:
            if hook is not None:
                self._call_hook(hook, args)
            self._drain()
        finally:
            self._leave(entered)
            self._arm(self._now_ps)
        self._finish()

    def _call_hook(self, hook: Callable[..., None], args: tuple[Any, ...]) -> None:
        try:
            hook(*args)
        except Exception as error:
            self.call_exception_handler({"message": str(error), "exception": error})

    def _enter(self) -> tuple[Any, Any, Any]:
        ident = _get_ident()
        previous = _events._get_running_loop()
        task = None if previous is None else _tasks._swap_current_task(previous, None)
        _events._set_running_loop(self)
        saved_hooks = None
        if self._hooks_thread != ident:
            if _core.on_host_thread():
                self._hooks_thread = ident
            else:
                saved_hooks = sys.get_asyncgen_hooks()
            sys.set_asyncgen_hooks(firstiter=_first_iteration, finalizer=None)
        self._stepping = ident
        return previous, task, saved_hooks

    def _leave(self, entered: tuple[Any, Any, Any]) -> None:
        previous, task, saved_hooks = entered
        self._stepping = None
        if saved_hooks is not None:
            sys.set_asyncgen_hooks(*saved_hooks)
        _events._set_running_loop(previous)
        if previous is not None:
            _tasks._swap_current_task(previous, task)

    def _drain(self) -> None:
        ready = self._ready
        now = self._now_ps
        while True:
            timers = self._timers
            while timers and timers[0][0] <= now:
                handle = heapq.heappop(timers)[2]
                cast(Any, handle)._scheduled = False
                if handle.cancelled():
                    self._cancelled_timers -= 1
                else:
                    ready.append(handle)
            if not ready:
                return
            next_handle = ready.popleft()
            if next_handle.cancelled():
                continue
            self._callbacks += 1
            if self._callbacks > MAX_CALLBACKS_PER_STAMP:
                self._overrun = ModelError(
                    f"{self!r}: {MAX_CALLBACKS_PER_STAMP} callbacks at one stamp without quiet,"
                    f" the next {next_handle!r}"
                )
                self._callbacks = 0
                ready.appendleft(next_handle)
                return
            cast(Any, next_handle)._run()

    def _task_done(self, task: asyncio.Task[Any]) -> None:
        self._tasks.pop(task, None)
        if cast(Any, task)._log_traceback:
            self._failed.append(task)

    def _settle_failed(self) -> None:
        failed = self._failed
        if not failed:
            return
        self._failed = []
        for task in failed:
            state = cast(Any, task)
            if state._log_traceback:
                state._log_traceback = False
                self.call_exception_handler(
                    {
                        "message": f"task {task.get_name()} failed and nothing retrieved its"
                        " exception by the end of its step",
                        "exception": task.exception(),
                        "task": task,
                    }
                )

    def _finish(self) -> None:
        if self._close_pending:
            self._close_pending = False
            self._close_now()
            return
        self._settle_failed()
        self._arm(self._now_ps)
        if self._overrun is not None:
            overrun = self._overrun
            self._overrun = None
            self._errors.clear()
            raise overrun
        if self._errors:
            self._raise_errors()

    def _raise_errors(self) -> NoReturn:
        first, *others = self._errors
        self._errors = []
        cause = first.get("exception")
        if isinstance(cause, ModelError):
            error = cause
        else:
            error = ModelError(f"{self!r}: {first.get('message')}: {cause!r}")
            error.__cause__ = cause
        for other in others:
            error.add_note(f"then {other.get('message')}: {other.get('exception')!r}")
        raise error

    def _note_asyncgen(self, agen: Any) -> None:
        self._asyncgens[agen] = None
        if len(self._asyncgens) > self._asyncgen_sweep:
            self._asyncgens = {a: None for a in self._asyncgens if a.ag_frame is not None}
            self._asyncgen_sweep = max(_ASYNCGEN_SWEEP_FROM, 2 * len(self._asyncgens))

    def _rewound(self) -> None:
        """The machine restored a snapshot: stamps count from its clock again."""
        port = self._port
        assert port is not None
        self._now_ps = port.board_time_ps()
        self._stamp_counted = -1

    def _stepping_here(self) -> bool:
        """Whether a step runs on the calling thread."""
        return self._stepping == _get_ident()

    def _stamp_since_reset(self) -> int:
        """The stamp of the running step in `Machine.time`'s base."""
        return self._now_ps - self._base_ps

    async def shutdown_asyncgens(self) -> None:
        """Close the async generators left unfinished."""
        for agen in list(self._asyncgens):
            await agen.aclose()
        self._asyncgens.clear()

    async def shutdown_default_executor(self, timeout: float | None = None) -> None:
        """Refused: the loop has no executor."""
        self._refused("shutdown_default_executor")

    def _refused(self, name: str) -> NoReturn:
        raise VirtualLoopUnsupported(
            f"VirtualLoop.{name}: a virtual-time loop never blocks and has no I/O, executor, signal"
            " or wall clock, and it runs only when its machine runs"
        )

    def run_forever(self) -> None:
        """Refused: the machine's run drives the loop."""
        self._refused("run_forever")

    def run_until_complete(self, future: Any) -> Any:
        """Refused: the machine's run drives the loop."""
        if isinstance(future, Coroutine):
            cast(Coroutine[Any, Any, Any], future).close()
        self._refused("run_until_complete")

    def stop(self) -> None:
        """Refused: the machine's run drives the loop."""
        self._refused("stop")

    def call_soon_threadsafe(self, callback: Any, *args: Any, context: Any = None) -> Any:
        """Refused: a post at a wall-clock instant is not deterministic."""
        self._refused("call_soon_threadsafe")


def _refusal(name: str) -> Callable[..., Any]:
    def refused(self: VirtualLoop, *args: object, **kwargs: object) -> Any:
        for argument in args:
            if isinstance(argument, Coroutine):
                cast(Coroutine[Any, Any, Any], argument).close()
        self._refused(name)  # pyright: ignore[reportPrivateUsage]

    refused.__name__ = name
    refused.__qualname__ = f"VirtualLoop.{name}"
    refused.__doc__ = "Refused: a virtual-time loop has no I/O, executor or signal."
    return refused


for _name in (
    "add_reader",
    "remove_reader",
    "add_writer",
    "remove_writer",
    "add_signal_handler",
    "remove_signal_handler",
    "run_in_executor",
    "set_default_executor",
    "getaddrinfo",
    "getnameinfo",
    "create_connection",
    "create_server",
    "create_unix_connection",
    "create_unix_server",
    "create_datagram_endpoint",
    "connect_read_pipe",
    "connect_write_pipe",
    "connect_accepted_socket",
    "subprocess_exec",
    "subprocess_shell",
    "sendfile",
    "start_tls",
    "sock_recv",
    "sock_recv_into",
    "sock_recvfrom",
    "sock_recvfrom_into",
    "sock_sendall",
    "sock_sendto",
    "sock_connect",
    "sock_accept",
    "sock_sendfile",
):
    setattr(VirtualLoop, _name, _refusal(_name))
del _name
