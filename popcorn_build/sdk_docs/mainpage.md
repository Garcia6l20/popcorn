# popcorn plugin SDK {#mainpage}

The C++ API for native plugins: peripheral models and bus devices built against popcorn's
headers and loaded at run time.

Start here:

- @ref popcorn::periph::model, the interface a peripheral model implements.
- @ref popcorn::periph::model_context, what a model is built with.
- @ref popcorn::periph::register_file, the register storage a model reads and writes.
- plugin/plugin.hpp, the plugin entry point `POPCORN_PLUGIN`.
- plugin/bindings.hpp, the nanobind helpers for bus devices.
- @ref popcorn::periph::spi_target, the interface of an SPI device.

The class list has every class and the file list every header of the reference.
