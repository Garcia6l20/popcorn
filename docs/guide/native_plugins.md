# Native plugins

A native plugin adds C++ peripheral models and C++ bus devices, built against popcorn's C++ API
and loaded at run time. Use one when a Python model is too slow, or when the model is C++ code
you already have. The example in `examples/stm32u575/native_plugin` serves the RNG from C++ and
an SPI ADC as a device. Its sources are the reference for this page.

## Use a plugin

A machine description declares the plugin and selects its models by name:

```python
--8<-- "guide/test_native_plugins.py:describe"
```

`Plugin.dev(directory)` is a source directory holding `popcorn-plugin.py`. `PLUGIN.model("rng")`
selects the model the plugin registers as `rng`, for the one peripheral that names it.

```python
--8<-- "guide/test_native_plugins.py:run"
```

`machine()` refuses a model name the plugin does not register and lists the names it does:

```python
--8<-- "guide/test_native_plugins.py:unknown"
```

## Write a plugin

The plugin directory holds the build script, the model and device sources and the bindings. The
build script is a pcons script:

```python
--8<-- "stm32u575/native_plugin/plugin/popcorn-plugin.py"
```

The model registers itself under a name with `POPCORN_PLUGIN`:

```cpp
--8<-- "stm32u575/native_plugin/plugin/rng.cpp:207:212"
```

A device is a C++ class bound with nanobind. The bindings module declares it:

```cpp
--8<-- "stm32u575/native_plugin/plugin/bind.cpp:9:15"
```

`claims()` of a model names its registers, `on_read` and `on_write` serve them, and
`ctx.events.schedule_at` schedules work in virtual time. `save` and `load` carry the state a
snapshot keeps. `plugin/saved.hpp` holds the helpers both models of the example use. The C++ API is documented in the
[Plugin SDK reference](../reference/sdk/index.md).

## Cost

A guest access to a plugin model stays native. Measured on an AMD Ryzen AI 7 PRO 350, Python 3.14.8, GCC 16.2.1, release `-O2` build,
a guest loop of 10^6 RNG DR reads with DRDY clear, the cheapest model path, three sessions of
11 runs, session medians: 53.5 to 54.8 ns per read with the plugin model, 1150 to 1179 ns with
the Python model, 38.4 to 39.5 ns for an RNG register no model claims. A whole run of the
python_model firmware to its 8th word took 0.63 to 0.72 ms with the plugin model, 0.89 to
1.11 ms with the Python model.

Of the 37 ns every guest MMIO access pays, the access trace record takes most. A plugin read
adds 15 ns of dispatch. A Python model read adds about 110 ns to take and release the GIL,
80 ns to enter Python and about 870 ns of interpreted code: `Model` dispatch, then the
handler's `regs` and `fields` lookups. A whole run gains less because Unicorn translating the
firmware into a fresh machine takes half of it, and each of the run's 56 Python calls costs
about 3 us on a machine thread that starts with cold caches. The bench
`tests/python/native_plugin/bench_native_model.py`
measures both, run it with `POPCORN_BENCH=1 uv run python -m pytest -s` on that file.


## Requirements

The plugin builds with the GCC 16 build popcorn was built with, the compiler `sdk.json` names
in the installed package's `popcorn/sdk/`. Dev mode also needs pcons and ninja, which the
`popcorn[plugin]` extra installs. The build uses popcorn's language and ABI flags from the SDK
and drops `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS`, `LDLIBS` and every `PCONS_*`
variable of the environment.
Warnings are the compiler's defaults, popcorn's warning set and `-Werror` are not applied.

## Selection

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

## Devices and `PLUGIN.module()`

`PLUGIN.module()` builds and loads the plugin, then imports its bindings module as
`popcorn_plugins.<name>._native`. Take device classes from it, never from a direct import of
the bindings module, dev or installed. A direct import maps the library before the loader
checks its ABI tag, and `MachineDescription` refuses a device class whose module
`PLUGIN.module()` did not import.

## Dev mode and the cache

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

## Packaged mode

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

## The ABI tag

There is no ABI stability across popcorn builds. Each plugin embeds the tag of the popcorn it
was built against, and the loader refuses any other, before the plugin's static constructors
run. A dev plugin rebuilds under a new key when popcorn changes. A mismatch reads:

```text
config error: .../lib/libmy_plugin.so:0: abi_tag, expected the tag of this popcorn
"popcorn 0.1.0.dev0;gcc 16.2.1 20260810;gxx-abi 1021;reflection 202603L;contracts 202502L;
cxx11-abi 1;glibcxx-debug 0;nanobind 2.13.0;nb-domain ;variant release;sdk 2b69e2f7...",
not "popcorn 0.0.0;gcc edited"
```

## Access attributes and snapshots

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

## Rules

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
