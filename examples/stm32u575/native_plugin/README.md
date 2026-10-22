# A C++ model and a C++ device from a native plugin: the RNG and an SPI ADC, HAL

A native plugin is a shared library built against popcorn's C++ API and loaded at run time. It
provides peripheral models, `popcorn::periph::model`, and bus devices bound with nanobind,
such as a `popcorn::periph::spi_target` subclass. A machine description declares it in
`plugins=`, names its models on the board and attaches its devices.

This example joins the python_model and spi_mock examples, in C++. The firmware first asks the
RNG for eight random words, one `HAL_RNG_GenerateRandomNumber_IT` at a time, stored from
`HAL_RNG_ReadyDataCallback`. Then it converts both channels of a two-channel 12-bit ADC on
SPI1, chip select PB0, 16 times with `HAL_Delay(1)` between rounds, two 16-bit frames per
conversion.

- `plugin/rng.cpp` serves CR and DR as `rngmodel.Rng` does with seed 1. RNGEN starts a
  conversion of 40 us of virtual time, DRDY and the RNG interrupt follow, each DR read returns
  the next xorshift32 word. A conversion puts its word in DR, a read empties DR again, so a
  debugger peek of DR shows what the next guest read returns. A CONDRST write cancels the
  conversion in progress and clears SR and DR, and clearing CONDRST with RNGEN set starts a
  new one.
- `plugin/adc.cpp` answers as `spiadc.Adc` with its default ramp: channel 0 rises
  `counts_per_ms` counts per millisecond of virtual time, channel 1 is its mirror.

## Run

From the repository root, with the `popcorn[plugin]` extra installed:

```sh
uv run pytest -q examples/stm32u575/native_plugin
```

The first run builds the plugin into the plugin cache, 3.1 to 3.2 s into an empty cache on an
AMD Ryzen AI 7 PRO 350 with 16 threads. Later runs reuse it until a source file, popcorn or the
compiler changes.

## Read first

- `conftest.py`: the plugin, its model on RNG and its device on SPI1.

  ```python
  PLUGIN = Plugin.dev(HERE / "plugin")

  class Adc(PLUGIN.module().AdcBase):
      def __init__(self, cs: str | None = None, counts_per_ms: int = 10) -> None:
          super().__init__(counts_per_ms)
          self._cs_port, self._cs_pin = chip_select(cs)

  popcorn.MachineDescription(
      ...,
      plugins=[PLUGIN],
      board={
          "RNG": popcorn.Periph(model=PLUGIN.model("rng")),
          "SPI1": popcorn.Periph(devices=[popcorn.Device(Adc, cs="PB0", counts_per_ms=25)]),
      },
  )
  ```

  RNG gets the plugin model because the board names it. SPI1 keeps its built-in model and
  carries the plugin's device. `popcorn.devices.chip_select` turns `cs` into the `_cs_port` and
  `_cs_pin` attributes the machine reads when it attaches an SPI device. A plugin ships no
  stub, so the conftest declares `AdcBase` for pyright under `TYPE_CHECKING`.
- `plugin/popcorn-plugin.py`: the plugin's build script, a pcons script.

  ```python
  project = PluginProject("plugin")
  project.native(["rng.cpp", "adc.cpp"])
  project.bindings(["bind.cpp"])
  ```

- `plugin/rng.cpp`: the model and the entry point. `claims()` names CR and DR, `on_read` and
  `on_write` serve them, the register file serves SR and the rest. `ctx.events.schedule_at`
  schedules the end of a conversion in virtual time, `on_event` handles it. `ctx.irqs` holds
  the RNG interrupt line. `save` and `load` carry the state a snapshot keeps, one field after
  the other, so a snapshot holds no padding byte and two snapshots of one state are equal.
  `load` throws `popcorn::config_error` on bytes of another size, so a damaged snapshot is
  refused rather than half applied. `plugin/saved.hpp` holds the helpers both models use.

  ```cpp
  POPCORN_PLUGIN(plugin, r)
  {
    r.add_model("rng", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const& desc) {
      return std::make_shared<native_plugin::rng>(ctx, desc, native_plugin::first_seed);
    });
  }
  ```

- `plugin/adc.cpp`, `plugin/bind.cpp`: the device, `select` and `exchange` of
  `popcorn::periph::spi_target`, bound as `AdcBase` in the bindings module. Its counters are
  read-only properties, read them while the machine is parked.

  ```cpp
  NB_MODULE(_native, m)
  {
    popcorn::plugin::require_core(m);
    nb::class_<native_plugin::adc, popcorn::periph::spi_target>(m, "AdcBase")
        .def(nb::init<std::uint32_t>(), nb::arg("counts_per_ms"));
  }
  ```

- `test_native_plugin.py`: the python_model and spi_mock tests against the C++ plugin.
  - a machine built without the plugin, strict, gets the built-in RNG model's words,
    `rng_words(0, 8)`
  - the words the firmware received equal the xorshift32 sequence from seed 1
  - `HAL_RNG_Init` writes RNG_NSCR, which the description adds, so no unmodelled-register
    warning is logged
  - each later word comes one conversion after the last
  - a CONDRST write clears SR and cancels the conversion in progress
  - CONDRST written at 1 and then at 0, RNGEN kept set, gives a new DRDY one conversion later
  - the ADC samples follow the ramp in virtual time, channel 1 its mirror
  - one select and one deselect per conversion, each holding two 16-bit frames.
- `firmware/main.c`: the RNG requests, then the chip select and the two-frame conversion.

A plugin model takes no parameters from the board in v1, `PluginModel` is the plugin and a
name only, so the python_model `seeded` config has no counterpart here. The native ADC has no
`source=` function: a Python function on the machine thread would take the GIL per frame.

## Writing a plugin

### Requirements

The plugin builds with the GCC 16 build popcorn was built with, the compiler `sdk.json` names
in the installed package's `popcorn/sdk/`. Dev mode also needs pcons and ninja, which the
`popcorn[plugin]` extra installs. The build uses popcorn's language and ABI flags from the SDK
and drops `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS`, `LDLIBS` and every `PCONS_*`
variable of the environment.
Warnings are the compiler's defaults, popcorn's warning set and `-Werror` are not applied.

### Selection

A declared plugin registers nothing by itself. `PLUGIN.model("rng")` selects its model for the
one peripheral that names it, never by IP glob. A board model, from a plugin or a Python
`popcorn.model.Model`, wins over the global registry, which holds the built-in models and the
`popcorn.model.register()` classes. `machine()` refuses a model name the plugin does not
register, listing the names it does.

`Periph.model` is `Use | PluginModel | None`. Code reading `.cls` or `.params` narrows first:

```python
model = description.board["RNG"].model
if isinstance(model, popcorn.Use):
    print(model.cls, model.params)
```

### Devices and `PLUGIN.module()`

`PLUGIN.module()` builds and loads the plugin, then imports its bindings module as
`popcorn_plugins.<name>._native`. Take device classes from it, never from a direct import of
the bindings module, dev or installed. A direct import maps the library before the loader
checks its ABI tag, and `MachineDescription` refuses a device class whose module
`PLUGIN.module()` did not import.

### Dev mode and the cache

`Plugin.dev(directory)` is a source directory holding `popcorn-plugin.py`. It builds on first
use into a cache entry keyed by every source file, the SDK and popcorn's ABI tag. Under pytest
the plugin of a `conftest.py` builds when its directory is collected, once for all xdist
workers, and a build failure is that directory's collection error.

A conftest that calls `PLUGIN.module()` at import, as this one does, builds at import instead,
and a failure there is an import error of that conftest. `pytest examples/stm32u575/native_plugin`
loads it before collecting and stops with "ImportError while loading conftest", exit code 4.
Run within a larger tree, the directory fails to collect: plain pytest reports 1 error and
runs nothing, exit code 2. Under xdist each worker reports the error, the build itself runs
once, and the other directories still run, exit code 1.

- The cache root is `POPCORN_PLUGIN_CACHE`, else `$XDG_CACHE_HOME/popcorn/plugins`, else
  `~/.cache/popcorn/plugins`. A root inside the plugin directory is refused.
- The copy and the key leave out hidden files and directories, `__pycache__`, `*.pyc`, `*~`
  and `#*#`. A plugin source or header with such a name is not built.
- An `#include` outside the plugin directory, the SDK and the compiler's own directories is
  refused.
- A build runs at most 6 jobs. Two entries per plugin name are kept.
- A loaded plugin is never unloaded. Editing its sources after it loaded raises ConfigError
  in that process: restart it.

### Packaged mode

A wheel built by pcons's backend, `pcons.pyproject`, installs the plugin with popcorn. The
backend runs a script named `pcons-build.py`, a dev-mode build runs `popcorn-plugin.py`.
`PluginProject.wheel()` does nothing outside a wheel build, so a project used both ways carries
both scripts with the same text:

```python
project = PluginProject("my_plugin")
project.native(["model.cpp"])
project.bindings(["bind.cpp"])
project.wheel()
```

```toml
[project]
name = "my-plugin"
dependencies = ["popcorn==0.1.0"]

[project.entry-points."popcorn.plugins"]
my_plugin = "my_plugin:PLUGIN"
```

`my_plugin/__init__.py` defines `PLUGIN = Plugin.installed("my_plugin", Path(__file__).parent /
"libmy_plugin.so", bindings="my_plugin._native")`, and `popcorn.plugin.find("my_plugin")`
returns it. The dependency pins popcorn exactly, else the build fails with ConfigError key
`project.dependencies`. A packaged plugin needs a bindings module: `wheel()` without
`bindings()` before it raises ConfigError key `bindings`, the backend refuses a wheel with no
extension module. A model-only plugin works in dev mode only.

### The ABI tag

There is no ABI stability across popcorn builds. Each plugin embeds the tag of the popcorn it
was built against, and the loader refuses any other, before the plugin's static constructors
run. A dev plugin rebuilds under a new key when popcorn changes. A mismatch reads:

```text
config error: .../lib/libmy_plugin.so:0: abi_tag, expected the tag of this popcorn
"popcorn 0.1.0.dev0;gcc 16.2.1 20260810;gxx-abi 1021;reflection 202603L;contracts 202502L;
cxx11-abi 1;glibcxx-debug 0;nanobind 2.13.0;nb-domain ;variant release;sdk 2b69e2f7...",
not "popcorn 0.0.0;gcc edited"
```

### Access attributes and snapshots

Each `on_read` and `on_write` gets a `periph::access_ctx` with the transaction's attributes:

- `secure` is the transaction's security attribute, whatever window it came through. It is
  false for every access, CPU, DMA or debugger, on a machine without TrustZone.
- `via_secure_alias` is true for an access through a `SEC_` alias window, with or without
  TrustZone. A model that read `secure` for the alias reads `via_secure_alias` now.
- `privileged` is false for an unprivileged CPU access. DMA and debugger accesses are
  privileged unless their master says otherwise.

Two `periph::model` members change how the machine treats a model:

- `stateless()` returning true says the model keeps no state beyond its register storage. A
  snapshot holds no bytes for it, `save` and `load` are never called, and it restores from the
  register storage. A save made by a machine with no model for that peripheral then loads too.
- `storage_only(offset)` returning true marks a claimed register as storage the model does not
  model. A host view access to it raises ModelError, as for a register of a peripheral without
  a model. Peek and poke still reach it.

### Rules

- One C++ namespace per plugin. The plugin library keeps default visibility, and two plugins
  with a class of one qualified name share a typeinfo name, so nanobind resolves the second
  binding to the first.
- Register models only through `r.add_model`. The loader refuses a plugin whose static
  constructors or init call `periph::register_model` or `arch::register_arch`.
- A static constructor must not call `plugin::load`, the refusal is a throw out of a static
  initializer and ends the process. It must not call a built-in's `register_builtin_models`
  either: every registration then returns silently and spends the once flag with nothing
  registered.
- A plugin library loads through `PLUGIN.module()` or `machine()` before anything else maps
  it, its bindings module included. A library mapped first is refused, its static
  constructors ran unguarded.
- Do not add `_GLIBCXX_DEBUG`, `-fno-rtti` or `-fno-exceptions`. The ABI tag does not see
  them, and model objects and exceptions cross the library boundary.
- A plugin's code runs on the machine thread like a built-in model: no Python, no blocking.

The guards catch mistakes, they are not a sandbox. Not detected: a model factory registering
globally when a machine builds, a library that links `libpopcorn_core.so` and also carries a
static core hidden by `-Wl,--exclude-libs,ALL`, and a bindings module with a static core when
`libpopcorn_core.so` sits in the global scope, for example after
`sys.setdlopenflags(os.RTLD_GLOBAL)` before `import popcorn`.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/native_plugin/native_plugin.elf examples/stm32u575/native_plugin/prebuilt/
```
