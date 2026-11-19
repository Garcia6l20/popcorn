# Profiling firmware

`Machine.profile` counts where the firmware spends its instructions while a block of code runs:
per function, asleep, in each exception, on each peripheral register and in loops that repeat
the same pass. A native sink does the counting, so nothing crosses to Python per instruction. The
counts are the same on every run, times round down to the picosecond per clock span, and the
call tree follows the rules of `NOTES`. In the snippets, `s(1)` is one virtual second
from `popcorn.units`, and `idle_spin` and `idle_wfi` are machine fixtures of the example, each
restored at `main` from its boot snapshot.

```python
--8<-- "guide/test_profiling.py:profile"
```

The walk-through below uses the [idle_profile](examples.md#idle_profile) example: one FreeRTOS
firmware built twice, `idle_spin.elf` with an idle task that spins and `idle_wfi.elf` whose idle
hook calls `__WFI()`. Two tasks poll, one on the TIM16 status register and one on a RAM flag that
the TIM2 interrupt sets.

## Read a report

`report.text()` prints one table per analysis. For one virtual second of `idle_spin` it starts:

```text
profile from instruction 21,126, no boot in it: 1000.000 ms at 4,000,000 Hz, 16,000,000 Hz, 15,995,296 instructions of time, 15,995,296 executed, 0 cuts

== hotspots ==
function                                     self  self%    inclusive  incl%     calls
prvCheckTasksWaitingTermination         9,501,610  59.4%    9,564,512  59.8% 1,357,373
prvIdleTask                             5,429,491  33.9%   15,029,632  94.0%         0
flag_task                                 665,198   4.2%      670,348   4.2%         0
status_task                               286,277   1.8%      290,428   1.8%         0
```

The idle task and the function it calls on every pass take 93 % of the instructions. The sleep
table reports no sleep, and the loops table shows the idle loop as runs of identical passes. The
same numbers are fields of the `Report`:

```python
--8<-- "guide/test_profiling.py:read"
```

## Sleep instead of spinning

Profile the image with the idle hook over the same second:

```python
--8<-- "guide/test_profiling.py:wfi"
```

The core now sleeps over 90 % of the time. `wakes` names the exception whose entry ended each
sleep: the FreeRTOS SysTick tick and the TIM6 HAL tick. `popcorn.profiling.diff` gives the change
from one report to the other, table by table, as `b` less `a`. `spin` is the report of the first
profile, on `idle_spin`:

```python
--8<-- "guide/test_profiling.py:diff"
```

## Polls and loop runs

The two poll tasks show the two shapes the loops analysis tells apart. The `peripherals`
analysis shows the polled register on top of the CPU accesses, TIM16_SR:

```python
--8<-- "guide/test_profiling.py:loops"
```

A pass is identical to the one before when the whole register file and every MMIO read value are
equal at the loop head. RAM is not compared. `status_task` reads TIM16_SR on every pass, so its
runs end with `progress` when the read value changes at UIF. `flag_task` reads a RAM flag, so
every pass looks the same and only an exception, here the TIM2 interrupt that sets the flag, or
the end of the profile ends its runs. A run counts from its first identical pass to what ended
it.

## Analyses

`Machine.profile("hotspots", "sleep")` runs the analyses named, every one when none is. Names are
the values of `popcorn.profiling.Analysis`.

| Analysis | Reports |
|---|---|
| `hotspots` | Self and inclusive instructions per function, call counts and the call tree, from the executed blocks and a shadow call stack per process stack context, so each RTOS task keeps its own. |
| `sleep` | Sleeps in WFI, WFE and sleep on exit, their time, the longest, and the exception that woke each. Time with no block, exception or sleep record, such as a held core, is reported apart. |
| `irq` | Entries per exception, entries per millisecond, time from entry to exit with and without the exceptions that preempted it, tail chains and the most entries in a window of `BURST_WINDOW_PS`, 1 ms. |
| `peripherals` | Reads, writes and repeated writes per register, by CPU and by DMA. Accesses from Python or a debugger are left out. |
| `loops` | Runs of at least `MIN_PASSES` identical passes per loop head, their passes, time and what ended them. |
| `watchdog` | IWDG key, prescaler and reload writes, the timeout they give, the smallest margin between two reloads and the gaps longer than the timeout. Refreshes, starts and key writes count the whole profile. Margin, closest pc, timeout and gaps are exact for a constant LSI rate. Per stretch of constant PR and RLR and per refresh or start, the longest interval is kept, so a stretch lists one gap. PR and RLR writes, restores and resets each open a stretch, and the list of stretches holds as many as the write log limit, 4096 by default. `logged_until_ps` is set when more stretches open, and margin, closest pc, timeout and gaps then stop at that time. |

`milestones=` names symbols or addresses whose first executed block `Report.timeline` times from
the profile start, a boot timeline for a profile opened at the reset. `report.text()` ends with
a `notes:` section, the `NOTES` constant: the rules each analysis applies, such as how a tail call
counts.

## Time

The profile counts instructions and converts them to time piece by piece, at the core rate each
stretch ran at, so a clock switch inside the profile is exact. `Report.clocks` lists those rates
with the instructions and picoseconds each covered. Times are picoseconds, `elapsed_ps` is the
profile's whole length and every share is a part of it. A sleeping core counts one instruction of
time per core clock cycle, so `elapsed_ticks` holds the sleeps and `executed` does not.

A reset or a snapshot restore inside the profile is a cut: the counts carry on across it and
`cuts` counts it. A profile opens parked. Leaving its block takes the counts at once while
parked, and at the next block boundary when the block is left inside a free run: call `report()`
once the run halts or after `await profile.wait_closed()`.

## Save and compare

```python
--8<-- "guide/test_profiling.py:export"
```

- `to_json()` returns the whole report as JSON text, `Report.from_json` reads it back equal.
- `write_folded()` writes the call tree as folded stacks, for `flamegraph.pl` or speedscope. It
  needs the `hotspots` analysis.
- `diff(a, b)` compares two reports, such as a JSON report kept from an earlier build.

## Profile a test

```console
$ pytest tests/test_spi.py --popcorn-profile
$ pytest --popcorn-profile=hotspots,sleep tests/test_spi.py
```

`--popcorn-profile` profiles every machine fixture of every test and writes one JSON report per
machine and test to the `popcorn_profile_dir` ini option, `popcorn-profile` in the rootdir by
default. Write it as `--popcorn-profile=ANALYSES` or last on the command line: a bare flag before
a path takes the path as its ANALYSES. The `profile` keyword of the `popcorn` marker does it for
one test. See [Profile a test](testing.md#profile-a-test).

## Cost

A profile runs the firmware in the same emulated order, so the guest's instructions, stops and
times are those of an unprofiled run. Host time grows with the blocks executed. One virtual
second of `idle_spin`, 16 million instructions, took 0.05 s of wall time unprofiled, 0.35 s with
every analysis but `hotspots` and 0.40 s with every one, measured on one x86_64 host. `hotspots`
adds the call stack tracker.

## Traces

A `.pctr` trace from `Machine.trace` carries the records a profile counts, for reading in
Python or in a trace viewer. The files are version 2 of the format, which `popcorn.trace.read`
reads. Beside blocks, memory accesses, exceptions, calls and stops, they hold:

- `popcorn.trace.Sleep`, a sleep, its reason and the exception that woke it.
- `popcorn.trace.Clock`, the core rate from a count on.
- `popcorn.trace.Loop`, a run of identical passes, with the `popcorn.trace.LoopEnder` that ended
  it. A trace gets loop records while a profile with the `loops` analysis is open on the
  machine.
- `popcorn.trace.Cut`, a block left before its last instruction, with the instructions it did
  not run.
- `popcorn.trace.Rebase`, a reset or a snapshot restore that moved the instruction count. Records
  after it count from there.
- `popcorn.trace.Mmio`, a register access, whose `initiator` is `cpu`, `dma` or `debugger`.

`popcorn.trace.to_chrome` converts a file for a Chrome trace viewer. See
[Machines](../reference/python/machines.md) for the reader.
