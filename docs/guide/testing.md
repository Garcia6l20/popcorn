# Testing with pytest

The `popcorn.pytest` plugin boots each machine description once per process and restores the
boot snapshot before every test that uses it. A test starts from the same state each time, at
the cost of a restore, not a boot.

pytest loads the plugin through its entry point wherever popcorn is installed with the `pytest`
extra. No conftest line is needed.

## Declare a machine fixture

```python
--8<-- "guide/test_testing.py:fixture"
```

`@popcorn.pytest.machine` turns a function that returns a `MachineDescription` into a fixture
named after the function. A test that takes a `button` parameter gets a booted `popcorn.Machine`.
Declare fixtures in a `conftest.py` to share them across test files.

## Write tests

Name the file `test_*.py` and run it as usual:

```sh
uv run pytest tests/
```

`ELF` in the snippets is the path of your firmware, see [First machine](first_machine.md). A helper drives the
button through the GPIO pin view. The button has a pull-up, so the pin rests high. Driving it
to 0 is the falling edge that fires the interrupt, and driving it back to 1 releases it:

```python
--8<-- "guide/test_testing.py:press"
```

Each test starts from the boot snapshot, whatever the test before it did:

```python
--8<-- "guide/test_testing.py:restored"
```

A coroutine test runs on the plugin's event loop. pytest-asyncio is not needed. Before each test
the plugin also removes the hooks the previous test installed and empties its intercept logs and
trace ring.

## Bound every run with a marker

```python
--8<-- "guide/test_markers.py:marker"
```

`@pytest.mark.popcorn` sets the settings of every popcorn machine a test uses:

- `budget`, the default instruction budget of each `run`, `call` and wait. `budget=None` means
  no limit. It never acts as a wall-clock timeout.
- `strict`, whether a firmware access to an unmodelled peripheral raises `ModelError`.

Put it on a module as `pytestmark = pytest.mark.popcorn(budget=...)`. The closest marker wins
whole: a function's marker replaces its module's. On the command line, `--popcorn-budget` and
`--popcorn-strict` apply where no marker sets the value.

## Running on another part

`--popcorn-part=PART` builds every machine the plugin makes on the shipped part `PART` instead of
the part its description names: machine fixtures, system members and `machine_factory` calls, a
selected config included. `PART` is a part of the description's die. The image runs on it when it
fits that part's flash and its pins are bonded.

```sh
pytest --popcorn-part=STM32U575VGTxQ examples/stm32u575/usart
```

The report header names the part and the summary prints `N of M tests ran on PART`. A test
that builds its own `popcorn.Machine` from a part description keeps that description's part and
is not counted in N. The session exits with status 1 when the option is set, the run would
otherwise exit 0 and N is 0. A machine description that sets `description=` or `svd=` is
refused, as the replaced part would not match the file. A `PART` that no description directory,
shipped layer or ST install names is a usage error.

## More machines in one test

```python
--8<-- "guide/test_testing.py:factory"
```

The `machine_factory` fixture builds new machines that belong to one test. `await
machine_factory.booted(description)` returns a machine restored from the boot snapshot of that
description, shared with its fixtures. Use it from async tests. The sync call
`machine_factory(description)` does the same once the description has booted.
`machine_factory(description, booted=False)` returns a machine powered off and not reset, for a
test that boots it itself:

```python
--8<-- "guide/test_testing.py:sync"
```

A `MachineDescription` with `missing="skip"` skips the tests that use it when its ELF or an ST
file it needs is missing, instead of failing them. Use it for firmware a contributor may not have
built.

`@popcorn.pytest.system` declares a fixture for a linked system the same way, see
[Systems and links](systems.md).

## When a test fails

The report of a failing test has a "popcorn" section per machine: the last stop, the decoded
fault registers and the call stack.

- `--popcorn-ring=N` adds the last `N` records of the machine's trace ring: MMIO accesses,
  interrupts, calls and stops.
- `--popcorn-trace-on-fail=DIR` traces every machine fixture and keeps the trace of each failed
  test in `DIR`.

## Firmware coverage

`--popcorn-cov` counts the firmware lines, functions and branches that every machine of the session
runs, and prints a table at the end of the run:

```console
$ pytest --popcorn-cov
...
------------------------------ firmware coverage -------------------------------
Name                         Lines  Hit  Cover  Funcs  Cover  Branches  Cover
-----------------------------------------------------------------------------
tests/firmware/api/callee.c    115   11   9.5%     32  15.6%         8   0.0%
tests/firmware/api/driver.c     38   11  28.9%     10  20.0%         6  33.3%
-----------------------------------------------------------------------------
TOTAL                          153   22  14.3%     42  16.6%        14  14.2%
```

`--popcorn-cov-report=KIND[:TARGET]` picks the reports and repeats. The options below have ini
options that hold their defaults:

| Option | Ini option | Value |
|--------|------------|-------|
| `--popcorn-cov-report` | `popcorn_cov_report` | `term`, `html[:DIR]`, `xml[:FILE]`, `lcov:FILE` or `gcovr:FILE`, one per line in the ini file |
| `--popcorn-cov-include` | `popcorn_cov_include` | glob of the source files to keep |
| `--popcorn-cov-omit` | `popcorn_cov_omit` | glob of the source files to drop |
| `--popcorn-cov-fail-under` | `popcorn_cov_fail_under` | percent from 0 to 100 |
| `--popcorn-cov-unmodelled` | `popcorn_cov_unmodelled` | `hit`, `mark` or `miss` |
| none | `popcorn_cov_lcov` | path of an lcov file, an implicit `lcov:` report |

```toml
--8<-- "guide/coverage_pyproject.toml"
```

The flag turns counting on. A `--popcorn-cov-*` option without it is a usage error, and the ini values
do nothing without it. A command-line
value replaces the ini value of its setting. Command-line targets resolve against the directory
pytest ran in, ini targets against the rootdir.

### Reports

- `term` is the table above, printed once with `-q` too and once under xdist. A bare
  `--popcorn-cov` with no other report and no `popcorn_cov_lcov` writes it.
- `html` writes into `htmlcov`, or the target directory.
- `xml` writes Cobertura xml to `coverage.xml`, or the target file.
- `lcov` writes the lcov tracefile, the format of `lcov` and `genhtml`. A bare `lcov` takes the
  path of `popcorn_cov_lcov`.
- `gcovr:FILE` writes a gcovr JSON tracefile, see [gcovr reports](#gcovr-reports). It needs a
  target.

Each process adds its counts to a temporary directory, and the controller builds the reports
from them once, at the end of the session. Under xdist the workers build none.

### gcovr reports

`--popcorn-cov-report=gcovr:cov.json` writes the coverage as a gcovr JSON tracefile. gcovr
builds its html pages from this tracefile, with a line and a branch column, the counts per line,
a function table and the summary with thresholds:

```console
$ python -m pytest --popcorn-cov --popcorn-cov-report=gcovr:cov.json
$ gcovr --root <rootdir> --json-add-tracefile cov.json --html-details out/cov.html
```

- gcovr is installed by the user. The tracefile has `gcovr/format_version` `0.15`, which gcovr 8.8
  reads. gcovr reads only a tracefile of its own format version, so another release needs a
  popcorn that writes that version.
- A `file` is relative to the pytest rootdir for a source under it and as written otherwise.
  `--root <rootdir>` makes gcovr find the sources, and the same tracefile works from any working
  directory and from a moved copy of the tree given as `--root`. gcovr reports only the files
  under `--root`, add `--filter <dir>` for each directory outside it.
- Include, omit and `--popcorn-cov-unmodelled` apply as for the other reports. `mark` writes the
  counts, `miss` the zeroed counts, and both write the sidecar `cov.json.unmodelled.json`, which
  gcovr ignores.
- A branch that never ran has a count of 0. Each branch carries its block and its index on the
  line. A line inside a function carries the function name.
- Tracefiles of several sessions merge with one `--json-add-tracefile` each. gcovr sums the line,
  branch and function counts. A function that starts on different lines in two tracefiles is a
  merge error, `--merge-mode-functions=separate` keeps both entries.
- gcovr marks a coverage percentage below `--html-medium-threshold` (75 by default) as low and
  below `--html-high-threshold` (90 by default) as medium.

### Include and omit

A glob matches a source path as the lcov file writes it and relative to the rootdir, and `**`
crosses directories. `**/tests/**` matches `tests/a.c` and `/home/u/x/tests/a.c`, where
`*/tests/*` matches neither. Include applies first, omit second. Every report, the lcov files
included, holds the kept files only.

### html and genhtml

`genhtml` from LCOV 2.0 or later, when it is on `PATH`, writes the `html` report with branch and
function coverage. It runs from the rootdir, where a relative source path resolves. Without
genhtml, a built-in renderer writes `index.html`, `style.css` and one page per source file,
with no scripts and no external files. `mark` mode uses the built-in renderer whenever a kept
line is marked.

The target directory takes the report when it is missing, empty or holds an earlier html report,
which is emptied first.
Any other target is left alone: a symbolic link, a directory of other files, and a directory that
is or contains the rootdir, the working directory or the home directory. The summary then says
`html report not written`, the other reports are still written, and the exit status is the run's.
A genhtml that fails or runs past 300 s gives the same summary line, and the `.pcov` files stay in
their directory.

`genhtml --branch-coverage` reads the lcov file with one message, which the data can legitimately
produce:

```text
line is hit but no branches on line have been evaluated
```

It names a line whose branch never ran while another instruction of the line did, such as a
shared exit that the line table files under the branch's line. `--ignore-errors inconsistent`
renders it. A function never entered can report as hit, `FNDA` 1, when the lines of an inlined
helper are filed inside its range: `genhtml` requires a function with a hit line to be hit.

### fail-under

`--popcorn-cov-fail-under=N` compares the total line coverage after include and omit with `N`
exactly, with nothing rounded first. Below it, the summary prints the failure and a run that
would exit 0 exits 1:

```text
FAIL Required coverage of 80% not reached. Total coverage: 79.99%
```

The printed total is cut to two decimals, so 79.999 prints 79.99. A run that
collected no coverage counts as 0 percent. An interrupted session skips the check.

### Unmodelled registers

A source line that reads or writes a peripheral register no model implements runs, but the
firmware did not exercise a model. `--popcorn-cov-unmodelled` sets how these lines report:

- `hit` counts them as covered and records nothing.
- `mark` keeps the counts, adds an `Unmodelled` column to the `term` table and colours the lines in
  the `html` report with a tooltip of `peripheral.register (reason, N hits)`. The reason is `no_register`,
  `storage_only`, `no_model`, `unimplemented` or `raz_wi`. Accesses from an instruction with no line in the report
  add up to `unplaced`, which the `term` report prints as
  `N unmodelled accesses came from code with no line in this report`.
- `miss` counts those lines, the functions whose only hit lines are among them and their branches
  as not hit in every report.

An `lcov` or `gcovr` report in `mark` or `miss` has a sidecar `<target>.unmodelled.json` that lists
the uses per line, and one that applies no sidecar removes the one at its path.

`mark` and `miss` make each machine record the CPU accesses per accessing instruction, about 20 ns
per access to a peripheral register, boot accesses included, and the mode is part of the boot cache
key. A line is marked when one of its instructions accessed an unmodelled register.

## Boot cache

Boot snapshots are cached in `.pytest_cache/d/popcorn/boot`. A later session, or another xdist
worker, restores a boot instead of running it. The cache key covers the popcorn build, the
description, the images and the model sources. `--cache-clear` drops the cache.

At session end, the plugin deletes the entries that can no longer hit: those booted from another
popcorn version or native extension, or from an image that changed or no longer exists. It also
deletes entries unused for `popcorn_cache_max_age_days` days, a cache hit counting as a use:

```ini
[pytest]
popcorn_cache_max_age_days = 14
```

The default is 7 days and 0 disables the age limit. Under xdist only the controller sweeps, and
an entry another process is booting or restoring is left for the next session. Files of a key
without a json, such as partial writes, are deleted once older than the age limit.
