#include <popcorn/core/errors.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/plugin/bindings.hpp>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nb = nanobind;

namespace
{
class echo_target final : public popcorn::periph::spi_target
{
public:
  void select(bool selected, popcorn::vtime) override
  {
    selects_ += selected ? 1U : 0U;
  }

  auto exchange(std::uint32_t out, std::uint8_t, popcorn::vtime) -> std::uint32_t override
  {
    ++frames_;
    return out + 1;
  }

  void save(std::vector<std::byte>& out) const override
  {
    auto const bytes = std::bit_cast<std::array<std::byte, sizeof frames_>>(frames_);
    out.insert(out.end(), bytes.begin(), bytes.end());
  }

  void load(std::span<std::byte const> in) override
  {
    if(in.size() != sizeof frames_)
    {
      throw popcorn::config_error("xmod", 0, "state", "the 4 bytes of an EchoTarget's save");
    }
    std::array<std::byte, sizeof frames_> bytes{};
    std::ranges::copy(in, bytes.begin());
    frames_ = std::bit_cast<std::uint32_t>(bytes);
  }

  [[nodiscard]] auto frames() const -> std::uint32_t
  {
    return frames_;
  }

  [[nodiscard]] auto selects() const -> std::uint32_t
  {
    return selects_;
  }

private:
  std::uint32_t frames_  = 0;
  std::uint32_t selects_ = 0;
};
}

NB_MODULE(_xmod, m)
{
  nb::module_::import_("popcorn._core");
  nb::class_<echo_target, popcorn::periph::spi_target>(
      m, "EchoTarget", "An SPI target bound outside _core: each frame back plus one.")
      .def(nb::init<>())
      .def_prop_ro("frames", &echo_target::frames, "Frames exchanged, saved in snapshots.")
      .def_prop_ro("selects", &echo_target::selects, "Chip select falling edges seen.");
  m.def(
      "throw_config", [] { throw popcorn::config_error("xmod", 0, "field", "expected"); },
      "Throw a popcorn::config_error from this module.");
  m.def(
      "registered_glob",
      [](std::string const& ip, std::string const& version) {
        return popcorn::periph::registered_glob(ip, version);
      },
      nb::arg("ip"), nb::arg("version"),
      "The registry glob serving this IP and version, None when none does.");
}
