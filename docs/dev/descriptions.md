# Descriptions

A part description says what a part is: its core, memory regions, peripherals with registers
and fields, interrupts, GPIO alternate functions, bonded GPIO pins and bonded ADC channels. The
machine is built from it and never from ad-hoc arguments.

## The data

The native type is `part_description` in `native/include/popcorn/core/description.hpp`. It
comes with `parse_description`, `load_description`, `dump_description`, `description_digest`
and a JSON Schema from `description_json_schema`. `description_schema_version`, 2, is part of
the data, and the digest covers it, so a bump changes every digest. The JSON Schema is committed
as `data/schema/part_description.schema.json` and `python/popcorn/stm32/description/_codegen.py`
generates the Python dataclasses of `_model.py` from it. Do not edit `_model.py` by hand.
`popcorn_build/schema.py` builds the `popcorn-describe-schema` program and rewrites the schema
from the native type.

## The pipeline

```mermaid
flowchart LR
    SVD[SVD] --> X[expand_svd]
    DB[CubeMX db] --> B[builder]
    R[rzone facts] --> B
    X --> B
    P[patch files] --> B
    H[CMSIS header] --> B
    B --> J[part description JSON]
    J --> C[build cache]
    J --> S[shipped layers]
```

The modules are in `python/popcorn/stm32/description`:

- `_sources.py` finds the SVD, the CubeMX db, the rzone facts, the GPIO modes file, the CMSIS
  device header and the patch files of a part. A CubeMX range stem such as `STM32L151C(6-8-B)Tx`
  is no part name. It resolves as `STM32L151C6Tx`, `STM32L151C8Tx` and `STM32L151CBTx`, each with
  its own flash and RAM size. A local build takes the rzone file of the db when it has one for
  the part and the packaged rzone facts file otherwise. The CMSIS header is `<line>xx.h`, or
  `<line><suffix>.h` for a part whose patch sets `[cmsis] suffix`.
- `_svd.py` expands an SVD: `derivedFrom`, `dim`, clusters and property inheritance. It is a
  pure function of the file bytes.
- `_cubemx.py` reads the part lookup, the memory regions and processor facts, the IP instance
  join and the GPIO alternate functions.
- `_patches.py` applies popcorn's own TOML patches: peripheral drops, fields, field drops, cross
  rules, aliases, IP renames and versions, register drops, IRQ changes, GPIO alternate function
  drops, core overrides, added registers and regions, and the device signature. The patches are in
  the `patches` directory.
- `_builder.py` assembles the description: expand the SVD, read the db and rzone, apply every
  patch, strip the peripheral prefix of register names, build the IRQ table, then assemble the
  core, regions and peripherals.
- `rzone` holds popcorn's own facts for memory regions and core counts, written from the
  reference manual and the datasheet.

The built description is cached as JSON under `$XDG_CACHE_HOME/popcorn/mcu`, keyed by the part,
the core, the schema version, the generator string and the digests of the resolved sources
in `_cache.py`.

## Peripheral drops and renames

A `[peripherals]` table removes SVD peripherals or gives them another name:

```toml
[peripherals]
renames = { SEC_LPUART1 = "SEC_LPUART" }
source = "the SVD names the Secure window of LPUART SEC_LPUART1"
```

`drop` lists names to remove, `renames` maps an SVD name to its new name, and the table carries
at least one of them and a `source`. Both apply first, before the IP renames and the aliases, so
the `[aliases]` rule pairs a renamed `SEC_<name>` with the peripheral `<name>` and every later
rule names the new name. A renamed peripheral must exist and not be dropped, each name is
renamed once across the files, and the new name must be free, otherwise the build raises
`DescriptionError` keyed `peripherals.renames.<name>`. The patch report counts them in
`peripherals_dropped` and `peripherals_renamed`.

## Part matching

A patch applies to a part when its `[match]` selects it. `parts` lists name globs, `svd` lists SVD
file name globs, `dies` lists die names and `flash_kib` lists flash sizes in KiB. Every key given
must select. `dies` and `flash_kib` read the part the name resolves to, so a group RefName
member has its own flash size, and a patch carrying either key never applies without that part.
One die holds several categories of one line, so die and flash size together select a category:

```toml
[match]
parts = ["STM32L15*"]
dies = ["DIE437"]
flash_kib = [512]
```

Patch files apply in file name order, the family file `stm32l1.toml` before its category files
`stm32l1_cat3.toml`, since `.` sorts before `_`. A family patch and a category patch may both
apply, the category patch after.

## IP renames, versions and register drops

`[ip] renames` maps a db IP instance to the SVD peripherals that take its IP and version. A key
that is no db instance of the part and a value that is no SVD peripheral are errors.
`renames_if_present` takes the same table and skips a key the part's db lacks and a value the SVD
or the drops removed, for families whose parts differ in instances. The report counts the
skipped pairs in `ip_renames_skipped`.

`[ip] versions` sets the IP version of every instance whose name matches the glob, after the
renames and `renames_if_present` of every patch, and needs a `source`:

```toml
[ip]
versions = { "FLASH" = "STM32L1_flash_cat46", "GPIO*" = "STM32L1_gpio_cat46" }
source = "RM0038 7.4.11: BRR exists on Cat.4 to Cat.6 only"
```

A glob names the instance as every `[peripherals] renames` spells it, matches at least one instance
and shares no instance with another glob of the table. `ip_version_changes` in the patch report
lists each instance with the CubeMX version it replaced and the new one. A rule's `source` names the
CubeMX version it replaces.

`[[register_drops]]` removes registers from a peripheral and from its aliases:

```toml
[[register_drops]]
peripheral = "RTC"
names = ["BKP20R", "BKP21R"]
source = "RM0038 25.6: Cat.1 has 20 backup registers"
```

A name the peripheral lacks is an error keyed `register_drops[<n>].names`, and a later rule
naming a dropped register fails as naming an unknown one.

## Alternate function drops

The CubeMX GPIO modes file can list an alternate function a part cannot route. A patch removes
it with an `[alternates]` table:

```toml
[alternates]
drop = [
  { peripheral = "GPIOH", pin = 2, af = 5, signal = "SPI5_SCK", source = "datasheet Table n" },
]
```

Each rule takes the port in `peripheral`, the `pin` and `af` numbers from 0 to 15, the `signal`
name and a non-empty `source` naming the document that justifies the drop. The entry must exist
on the port and no rule may repeat it, otherwise the build raises `DescriptionError` with the
rule's index. The builder applies the rules after it reads the CubeMX modes, validates all of
them before removing any, and counts the drops in the patch report.

## Bonded pins

Each GPIO port of a description built from CubeMX or pin data carries `bonded`, the mask of its
pins that reach a package pin, bit n for pin n. `bonded_pins` in `_cubemx.py` reads it from the
part's pin list. A port the package bonds no pin of carries 0 and keeps its registers. The
board API refuses a pin outside the mask with `ConfigError`: every `Pin` access, `drive_edges`
and an SPI device's chip select at machine construction. The firmware reaches every pin through
the registers. A hand-written description may leave `bonded` out, every pin of the port then
counts as bonded.

Each ADC instance carries `bonded_channels`, the mask of its external channels whose pad reaches
a package pin. `bonded_adc_channels` in `_cubemx.py` reads it from the `ADC<n>_IN<c>` and
`ADC<n>_INP<c>` signals of the part's pins. The board API refuses a channel outside the mask with
`ConfigError`: `ADCx.input(n)`, every `AnalogInput` access and its refs. Conversions read every
external channel. A hand-written description may leave `bonded_channels` out, every external
channel then counts as bonded.

## Field drops

An SVD can describe fields a part does not implement, such as the per-pin fields of pins a port
lacks. A `[[fields]]` rule with `drop = true` removes them:

```toml
[[fields]]
instances = ["GPIOI"]
registers = ["GPIOI_MODER"]
fields = ["MODE12", "MODE13", "MODE14", "MODE15"]
drop = true
source = "silicon read-back: GPIOI_MODER keeps 0x00FFFFFF of 0xFFFFFFFF"
```

The rule takes the usual `ip` or `instances` selector, `registers` with an optional `each`
range, the `fields` and a non-empty `source`. It carries no `width`, `on_write`, `on_read` or `acc`.
An `ip` selector picks the peripherals of that IP that are not aliases, `instances` must name
such peripherals. Each named register and field must exist and no rule may drop a field twice.
Otherwise the build raises `DescriptionError` with the rule's index. The fields leave the named
peripheral and each of its aliases, and a later rule naming them fails as naming an unknown
field. The patch report counts the drops in `fields_dropped`.

## Field renames

Two families can spell one field differently in their SVDs, such as the high bit of the timer
slave mode. A `[[fields]]` rule with `rename` gives the SVD field the spelling the model looks up:

```toml
[[fields]]
instances = ["TIM2"]
registers = ["TIM2_SMCR"]
rename = { SMS_2 = "SMS_1", TS_2 = "TS_1" }
source = "RM0481 Rev 5, 44.5.3: the SVD spells the SMCR bits as SMS_2 and TS_2"
```

`rename` maps each old name to its new one. The rule takes the usual selector, `registers` with an
optional `each` range and a non-empty `source`. It carries no `fields`, `drop`, `width`,
`on_write`, `on_read` or `acc`. An `ip` selector picks the peripherals of that IP that are not aliases,
`instances` must name such peripherals. The build raises `DescriptionError` with the rule's index
when a register or a field is unknown, when the new name is already a field of the register, when
a rule renames a field an earlier rule renamed, or when the rule names no field. The new spelling
applies to the named peripheral and each of its aliases, and a later rule names it. The patch
report counts the renames in `fields_renamed`.

## Field adds

Silicon can hold a field the SVD omits. A `[[fields]]` rule with `add` puts one field into an
existing register:

```toml
[[fields]]
instances = ["TIM15"]
registers = ["TIM15_SMCR"]
add = { name = "SMSPE", lsb = 24, width = 1, acc = "read_write", reset = 0 }
source = "Silicon read-back on an STM32H573: TIM15_SMCR written 0xFFFFFFFF reads 0x033100F7"
```

`add` holds the field `name`, its bit offset `lsb`, its `width`, its access `acc` and its `reset`
value. The reset value lands in the register's reset word and marks those bits as known. The rule
takes the usual selector, `registers` with an optional `each` range and a non-empty `source`. It
carries no `fields`, `drop`, `rename`, `width`, `on_write`, `on_read` or `acc`. The field is added to
the named peripheral and each of its aliases, and a later rule names it. The build raises
`DescriptionError` with the rule's index when the register is unknown, when the name is already a
field of the register, when the bits overlap another field or leave the register, when the reset
value does not fit the width, or when an earlier rule added the same field. The patch report
counts the adds in `fields_added`.

## Field effects

An SVD can give a field the wrong size, write effect, read effect or access, such as the USB
endpoint registers of the STM32H5, whose toggle bits the SVD calls write-only. A `[[fields]]` rule
with any of `width`, `on_write`, `on_read` and `acc` sets them on existing fields:

```toml
[[fields]]
ip = "USB"
registers = ["USB_CHEP{n}R"]
each = { n = [0, 7] }
fields = ["STATTX", "DTOGTX", "STATRX", "DTOGRX"]
acc = "read_write"
on_write = "one_to_toggle"
source = "RM0481 Rev 5, 61.6.7: STATTX, DTOGTX, STATRX and DTOGRX are read and toggled by writing 1"
```

`width` is a bit count, `on_write`, `on_read` and `acc` take the spellings of the description
JSON, such as `zero_to_clear`, `clear` and `read_only`. The rule takes the usual selector,
`registers` with an optional `each` range, the `fields` and a non-empty `source`. An `ip` selector
picks the peripherals of that IP that are not aliases, `instances` must name such peripherals.
Each value applies to the named peripheral and each of its aliases, so a secure alias keeps the
fields of its base. The build raises `DescriptionError` with the rule's index when a register or a
field is unknown, when an earlier rule set the same property of the same field, or when a new
width leaves the register or overlaps another field. The patch report counts one
`fields_touched` per property, field and peripheral or alias it sets. `fields_dropped`,
`fields_renamed` and `fields_added` count each base field once, whatever its aliases.

## Bit-band regions

A region of kind `bit_band` is a Cortex-M bit-band alias window. Its `physical` is the base of
the target range, and alias word `w` maps bit `w % 8` of target byte `w / 8`. A family patch adds
the windows with `[memory] add`:

```toml
[memory]
source = "RM0038 Rev 18, 2.6: bit-band regions of SRAM and of the peripherals"
add = [
  { name = "BB_SRAM", start = 0x22000000, size = 0x02000000, kind = "bit_band", acc = { read = true, write = true, execute = false }, secure = false, physical = 0x20000000 },
  { name = "BB_PERIPH", start = 0x42000000, size = 0x02000000, kind = "bit_band", acc = { read = true, write = true, execute = false }, secure = false, physical = 0x40000000 },
]
```

A `bit_band` region is one of the two architectural windows, 32 MB at 0x22000000 onto 0x20000000
or at 0x42000000 onto 0x40000000, with no content and no controller. Otherwise the build raises
`DescriptionError` keyed `memory.add.<name>`. A description file that places a window elsewhere is
refused when the machine is built if the window's target range overlaps any `bit_band` region.

The machine maps the window as MMIO and holds no bytes for it, so a snapshot carries no image of
it and a power-on reset leaves it alone. A CPU or debugger access of 1, 2 or 4 bytes reaches the
target unit of the same size that holds the bit: a read returns the bit as 0 or 1, a write reads
the unit, sets or clears the bit from bit 0 of the value and writes the unit back. Both target
accesses go through the bus with the attributes of the alias access, so watches, the firewall, the
peripheral models, read side effects, the trace ring and coverage see accesses at the target
address, and a write into memory holding translated code drops that code from the next block on.
The read and the write run inside one access at one instruction count, and DMA beats run between
instructions, so the read-modify-write is atomic. A DMA access to an alias address is a bus error.

## Data EEPROM

A CubeMX part file with an `<E2prom>` element gives the data EEPROM size in bytes, per expanded
name for a range stem. `[memory] eeprom` adds a read-only region `DATA_EEPROM` of that size at
`start`, with an optional `controller` peripheral:

```toml
[memory]
source = "RM0038 Rev 18, Table 8: data EEPROM at 0x08080000"
eeprom = { start = 0x08080000, controller = "FLASH" }
```

The size is a whole number of 1 KB pages, or the build raises `DescriptionError` keyed
`memory.eeprom`, as it does for a part without the element, for a range that overlaps another
region, and for a `controller` table entry naming `DATA_EEPROM` beside the `eeprom` one.

## CMSIS header names

`[cmsis] suffix = "xe"` in a patch names the CMSIS device header `<line>xe.h`, `stm32l152xe.h`
for `STM32L152RETx`. The suffix is `x` followed by at least one lowercase letter or digit. At most
one matching patch sets it. A part with no such patch uses `<line>xx.h`.

## Shipped descriptions

Shipped descriptions live in `python/popcorn/stm32/data` as three kinds of xz-compressed JSON
layer, each `{"format": 1, ...}`:

| file | holds |
|---|---|
| `<family>.blocks.json.xz` | `blocks`: every `registers` plus `cross_rules` pair of the family's shipped parts, keyed by the SHA-256 of its canonical JSON |
| `<line>.line.json.xz` | the `reference` part's `schema_version`, `core`, `irqs` and peripheral headers in order |
| `<line>.parts.json.xz` | `line_sha256` of the decompressed line file and `parts`: one delta per shipped part, `<RefName>[-<core>]` |

The family is the RefName's first 7 characters plus `xx`, `STM32U5xx`, the line its first 9
plus `xx`, `STM32U575xx`. A header is a peripheral without `registers` and `cross_rules`, with
`"block": <key>` in their place. Two instances share a block only when their bytes are equal. A
delta holds the part's `part`, `generator`, `sources` and `regions`, the full header of every
instance that differs from the line's, and `schema_version`, `core` or `irqs` where they differ.
Every part of a line has the line's instances in the line's order.

`_layers.py` holds `factor`, which writes the layers of a set of descriptions, and `compose`, its
inverse, both pure functions. `compose_shipped` in `_shipped.py` composes a part into the same
JSON text `dump_json` writes, so the digest, snapshots and the native loader see the same
description. It refuses with `DescriptionError` naming the file: a layer of another `format`, a
`line_sha256` mismatch, a missing block or one whose bytes do not hash to its key, an unknown
instance, and a decompressed layer or composed description above `MAX_SHIPPED_BYTES`. The
composed size is counted before anything is built. On first use the composed text is cached as
`<cache>/shipped/<name>-<sha16>.json`, sha16 over the SHA-256 of each of the parts, line and family files, in order.

A part ships exactly when its golden file `tests/data/stm32/golden/<RefName>[-<core>].json`
exists. To add a part, write its golden, then ship:

```
uv run python -m popcorn.stm32.description golden STM32U575QGIx --update
uv run python -m popcorn.stm32.description ship STM32U575QGIx --pin-data <STM32_open_pin_data> \
    --out python/popcorn/stm32/data
uv run python -m popcorn.stm32.description ship --pin-data <STM32_open_pin_data> --check \
    --out python/popcorn/stm32/data
```

`ship` builds every golden part of each family it touches from `STM32_open_pin_data`, in a pool
of `-j` processes, and rewrites all layers of those families. A part named on the command line
selects its family, no part selects every family with a golden or a layer file. Each line's
reference is its part with the most bonded GPIO pins, then the largest flash, then the first
RefName in sorted order. Every file goes to a temporary name first and is renamed once all are
written. Then every layer file of a touched family that the new set lacks is removed, so a part
whose golden is deleted leaves no delta and a line with no part leaves no file. Equal inputs give
equal bytes. Writing needs `--out`, the package data directory of the source tree: under
`uv run` the imported package is the staged copy in the build directory. `--core` applies to
every named part and needs a part list.

`ship --check` composes each shipped part, every golden part without a part list, and compares
its digest with a fresh build. `golden` without a part checks every golden file.

The lookup order of `popcorn.stm32.mcu(part)` is:

1. `<RefName>[-<core>].json`, uncompressed, in a directory of `POPCORN_STM32_DESCRIPTION_DIRS`,
   searched in order.
2. The shipped layers, skipped when `svd`, `cubemx_db` or `cmsis_header` is passed or
   `POPCORN_STM32_BUILD` is `local`. Any other value of that variable raises an error.
3. A local build from ST's files, cached.

The [Install](../guide/install.md) page shows the same order from the user side.

`_golden.py` writes a canonical text dump of a description, one line per item. The dump and its
digest file make a change of a shipped description visible in review.

## Digests

`description_digest` is a SHA-256 of the description content. A snapshot stores it and checks it
on load, so a snapshot only loads into a machine built from the same description.

## Sweeping parts

`popcorn_build.part_sweep` builds the description of many parts and attaches a Machine to each
at TZEN 0 and 1. Run it from the checkout:

```sh
uv run python -m popcorn_build.part_sweep --out sweep.json --parts 'STM32U5*' 'STM32H5*' -j 10
uv run python -m popcorn_build.part_sweep --out shipped.json --source shipped
```

- `--source local`, the default, builds every CubeMX db part matching the globs from ST's files.
  `--source shipped` loads the shipped parts.
- Each part runs in its own child interpreter, at most `-j` at once, 120 s each. A child that
  crashes or runs out of time gives a row with `error` set to `crashed` or `timeout`.
- The output is `{"format": 1, "cubemx_db": ..., "popcorn": <git describe>, "parts": [...]}`,
  rows sorted by part. A row holds the db facts of the part, `line`, `die`, `package`,
  `flash_kib`, `ram_kib` and `bonded`, the pin mask per GPIO port from `bonded_pins`, each null
  when the db lacks the part or its file does not read, the latter with `db_error` set. It also
  holds `smps` from `is_smps_part`, the description digest, the attach result per TZEN value,
  and the instance counts: total, with a built-in model at TZEN 0 and 1, and the names with no
  IP version.
- `--source local` takes the db's `mcu/STM32*.xml` files. A range stem such as
  `STM32C011F(4-6)Px` gives one row per expanded name, `STM32C011F4Px` and `STM32C011F6Px`, each
  with its own flash and RAM size. The run stops with an error when the db has no `mcu/`
  directory or no part matches.
- A failed stage holds the exception type and the first 300 characters of its text.
