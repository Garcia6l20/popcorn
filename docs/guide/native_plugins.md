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
--8<-- "stm32u575/native_plugin/plugin/bind.cpp:9:26"
```

`claims()` of a model names its registers, `on_read` and `on_write` serve them, and
`ctx.events.schedule_at` schedules work in virtual time. `save` and `load` carry the state a
snapshot keeps. `plugin/saved.hpp` holds the helpers both models of the example use. The C++ API is documented in the
[Plugin SDK reference](../reference/sdk/index.md).

## Cost

A guest access to a plugin model stays native. Measured on an AMD Ryzen AI 7 PRO 350, Python
3.14.8, GCC 16.2.1, release `-O2` build, a guest loop of 10^6 RNG DR reads with DRDY clear, the cheapest model path, three sessions of
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
`tests/python/native_plugin/bench_native_model.py` measures both, run it with `POPCORN_BENCH=1 uv run python -m pytest -s` on that file.

## Reference

The rules a plugin follows, from the example README:

--8<-- "stm32u575/native_plugin/README.md:118:260"
