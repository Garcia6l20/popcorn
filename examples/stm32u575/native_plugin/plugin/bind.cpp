#include "adc.hpp"

#include <popcorn/plugin/bindings.hpp>

#include <cstdint>

namespace nb = nanobind;

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
  nb::class_<native_plugin::adc, popcorn::periph::spi_target>(
      m, "AdcBase", "Native side of the example's Adc: the two-channel SPI ADC in C++.")
      .def(nb::init<std::uint32_t>(), nb::arg("counts_per_ms"))
      .def_prop_ro(
          "counts_per_ms", &native_plugin::adc::counts_per_ms, "The slope of the default ramp.")
      .def_prop_ro(
          "selects", [](native_plugin::adc const& self) { return self.counted().selects; },
          "Chip select falling edges.")
      .def_prop_ro(
          "deselects", [](native_plugin::adc const& self) { return self.counted().deselects; },
          "Chip select rising edges.")
      .def_prop_ro(
          "bad_frames", [](native_plugin::adc const& self) { return self.counted().bad_frames; },
          "Frames of another size than 16 bits, and selections without exactly two frames.");
}
