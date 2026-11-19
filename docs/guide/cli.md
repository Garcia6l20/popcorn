# The popcorn command

`popcorn` reads what popcorn writes: the JSON reports of `--popcorn-profile`, `.pcov` coverage
files and part descriptions. It runs from the shell and needs no script.

```console
$ uv run --no-sync popcorn --help
$ uv run --no-sync popcorn profile --help
```

The examples below run in a directory that holds `spin.json` and `wfi.json`, two reports of
[profiling](profiling.md), and the directory `reports` with the same two files. Output is
shown as printed, long outputs cut.

## profile

The `profile` commands take the JSON reports that `--popcorn-profile` writes to the
`popcorn_profile_dir` ini option, or that `Report.to_json` writes. Where a command takes a
directory, it reads the `*.json` files directly in it and skips `.json.part` files.

### `popcorn profile show`

Prints the text of each report, as `Report.text` does. Each argument is a report or a directory
of reports. With more than one report, each one starts with a `== path ==` line.

```text
--8<-- "guide/cli/show.txt"
```

The output goes on with the other analyses the report holds and the notes that explain them.

### `popcorn profile folded`

Writes the call tree as folded stacks for `flamegraph.pl` and [speedscope](https://www.speedscope.app).
A stack is the frames from the entry down, joined with `;`, then the self instructions.

```text
--8<-- "guide/cli/folded.txt"
```

Without `-o`, or with `-o -`, the stacks go to stdout. With a file, the command prints the
number of lines it wrote on stderr. A file literally named `-` is written as `-o ./-`.

### `popcorn profile chrome`

Writes the call tree as a V8 `.cpuprofile`. speedscope opens it as a file and the Performance
panel of Chrome DevTools opens it with Load profile.

```text
--8<-- "guide/cli/chrome.txt"
```

A node of the profile is a call tree frame and its `hitCount` is the frame's self instructions.
One time unit is one instruction, shown by the viewers as 1 us. The time axis lays the weights of
the call tree end to end. It is no recording of when the instructions ran and sleeps do not
appear. For a real timeline, convert a `.pctr` trace with `popcorn.trace.to_chrome`, see
[Traces](profiling.md#traces).

### `popcorn profile diff`

Prints what changed from report A to report B. Each value is B less A. Two directories are
matched by file name, the names found on one side only are listed, and the output ends with the
number of reports that did not change.

```text
--8<-- "guide/cli/diff.txt"
```

`--table` keeps one table and can be repeated. It filters the JSON output as well. `--top`
limits the rows per table of the text output and `0` prints all. `--json` prints the diff as
JSON. The command exits 0 whether or not the reports differ.

The tables are `summary`, `functions`, `frames`, `sleep`, `wakes`, `exceptions`, `registers`,
`loops`, `watchdog` and `timeline`. Rows sort by the absolute change of one column per table:
`self` for functions and frames, `self_time_ps` for exceptions, `time_ps` for wakes, loops and
sleep, `reads` for registers, else the first column by name.

### `popcorn profile summary`

Prints one row per report of a directory: instructions, time in ms, the share asleep, the share
in exception handlers and the function with the most self instructions. A `-` marks a section
the report does not hold.

```text
--8<-- "guide/cli/summary.txt"
```

`--sort` orders the rows by `name`, `executed`, `ms`, `sleep` or `irq`. Only `name` ascends.
`--functions N` adds the N functions with the most self instructions over all reports, with
their share of all instructions and the number of reports that hold them. `--json` prints the
table as JSON. A report that cannot be read is named on stderr and skipped, the table of the
others prints and the exit status is 1.

## coverage

The `coverage` commands work on `.pcov` files, the coverage of one or more runs.

### `popcorn coverage merge`

Sums `.pcov` files into one. The order of the inputs does not matter.

```text
--8<-- "guide/cli/merge.txt"
```

### `popcorn coverage export`

Converts a `.pcov` file. At least one of `--lcov`, `--drcov` and `--unmodelled` is needed.

```text
--8<-- "guide/cli/export.txt"
```

The lcov file holds branch records unless `--no-branches` is given, and `--test-name` sets its
`TN:` record. The drcov file is for Lighthouse and lists each block that ran, once. A block
outside the executable extent of every image is left out and the command says how many. The
`--unmodelled` file is the JSON sidecar of the source lines that accessed unmodelled registers.
The ELF files a `.pcov` was collected from are read from their recorded paths, and the export
fails when one changed or is gone.

## part

### `popcorn part explain`

Prints, for each peripheral instance of a part, its model, the row that serves it and where the
row comes from. `--trustzone` lists the TrustZone models.

```text
--8<-- "guide/cli/explain.txt"
```

The output continues with one line per instance. `python -m popcorn.stm32 explain` prints the same.

## Options

| Command | Option | Effect |
| --- | --- | --- |
| `profile folded` | `-o`, `--output FILE` | File to write, `-` for stdout |
| `profile chrome` | `-o`, `--output FILE` | `.cpuprofile` file to write, required |
| `profile diff` | `--top N` | Rows per table, default 10, 0 for all |
| `profile diff` | `--table NAME` | Keep one table, repeatable |
| `profile diff` | `--json` | Print JSON |
| `profile summary` | `--sort KEY` | `name`, `executed`, `ms`, `sleep` or `irq` |
| `profile summary` | `--functions N` | Also list N functions, default 0 |
| `profile summary` | `--json` | Print JSON |
| `coverage merge` | `-o`, `--output FILE` | `.pcov` file to write, required |
| `coverage export` | `--lcov FILE` | Write an lcov tracefile |
| `coverage export` | `--drcov FILE` | Write a drcov file |
| `coverage export` | `--unmodelled FILE` | Write the unmodelled-access JSON |
| `coverage export` | `--no-branches` | Leave branch records out of the lcov file |
| `coverage export` | `--test-name NAME` | `TN:` record of the lcov file |
| `part explain` | `--trustzone` | List the TrustZone models |

`--help` and `-h` work on every command, and `--version` on `popcorn`.

## Exit codes

- 0: the command did what was asked.
- 1: a file could not be read or written, a report or `.pcov` file is malformed, or a part is
  unknown. The message starts with `Error:` on stderr. `show` and `summary` print an `Error:` line
  per bad file, print the rest and exit 1. `show` also exits 1 on a missing path or an empty
  directory, and `summary` on an empty directory.
- 2: a usage error, such as a missing argument, an unknown option or a path click rejects. A
  missing input file of every command except `show`, a missing directory given to `summary` and a
  directory given as an output file are rejected this way.
