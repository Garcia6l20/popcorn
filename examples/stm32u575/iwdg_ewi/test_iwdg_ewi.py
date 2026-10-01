"""An IWDG with early wakeup, reloaded from a busy loop, keeps the event queue small.

The firmware starts the IWDG with prescaler 8, a 2048-count reload (512 ms from the 32 kHz LSI)
and an early wakeup 204 counts before the end, EWIE set, then reloads KR after every few dozen
instructions. Each reload moves the expiry and the early wakeup deadlines. `machine.pending_events`
counts the events waiting in the event queue: the watchdog adds at most two more than before the
start, its expiry and its early wakeup, however often the firmware reloads. The early wakeup
never fires.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=20_000_000)

IWDG_EVENTS = 2


@pytest.mark.xfail(strict=True, reason="iwdg: every KR reload leaves its replaced events queued")
async def test_reloads_keep_at_most_two_watchdog_events_queued(machine: Machine) -> None:
    stop = await machine.run(until="watchdog_starting", for_=ms(10))
    assert stop.reason is StopReason.ADDRESS, stop
    baseline = machine.pending_events
    stop = await machine.run(until="watchdog_configured", for_=ms(10))
    assert stop.reason is StopReason.ADDRESS, stop

    await machine.run(for_=ms(50))
    assert machine.var("reloads").value > 1000
    assert machine.pending_events <= baseline + IWDG_EVENTS
    assert machine.periph.IWDG.SR.EWIF == 0
