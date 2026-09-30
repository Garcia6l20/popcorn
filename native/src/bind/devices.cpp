#include "devices.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/trampoline.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace popcorn::bind::detail
{
namespace
{
namespace nb    = reflex::py::nb;
namespace stm32 = periph::stm32;

template <typename Trampoline, typename... Args>
auto call(Trampoline const& self, char const* name, Args&&... args) -> nb::object
{
  nb::detail::ticket ticket(self, name, true);
  return self.base().attr(ticket.key)(std::forward<Args>(args)...);
}

void append(nb::object const& result, std::vector<std::byte>& out)
{
  auto const bytes = nb::cast<nb::bytes>(result);
  auto const data  = static_cast<std::byte const*>(bytes.data());
  out.insert(out.end(), data, data + bytes.size());
}

/**
 * @brief nanobind trampoline letting a Python class implement stm32::i2c_target through the
 *        private methods `_address`, `_start`, `_write`, `_read`, `_stop`, `_reset`, `_save` and
 *        `_load` that `popcorn.board.I2CTarget` defines, which check the values returned. Every
 *        call takes the GIL. Times cross as picoseconds since power-on.
 */
class python_i2c_target final : public stm32::i2c_target
{
public:
  NB_TRAMPOLINE(stm32::i2c_target, 8);

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<std::uint8_t>(call(nb_trampoline, "_address"));
  }

  [[nodiscard]] auto start(bool read, vtime at) -> bool override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<bool>(call(nb_trampoline, "_start", read, at.ps));
  }

  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> bool override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<bool>(call(nb_trampoline, "_write", byte, at.ps));
  }

  [[nodiscard]] auto read(vtime at) -> std::uint8_t override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<std::uint8_t>(call(nb_trampoline, "_read", at.ps));
  }

  void stop(vtime at) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_stop", at.ps);
  }

  void reset(reset_kind kind) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    nb::gil_scoped_acquire gil;
    append(call(nb_trampoline, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_load", nb::bytes(in.data(), in.size()));
  }
};

/**
 * @brief nanobind trampoline letting a Python class implement stm32::spi_target through the
 *        private methods `_select`, `_exchange`, `_reset`, `_save` and `_load` that
 *        `popcorn.board.SpiTarget` defines, which check the values returned. Every call takes
 *        the GIL. Times cross as picoseconds since power-on.
 */
class python_spi_target final : public stm32::spi_target
{
public:
  NB_TRAMPOLINE(stm32::spi_target, 5);

  void select(bool selected, vtime at) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_select", selected, at.ps);
  }

  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<std::uint32_t>(call(nb_trampoline, "_exchange", out, bits, at.ps));
  }

  void reset(reset_kind kind) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    nb::gil_scoped_acquire gil;
    append(call(nb_trampoline, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    nb::gil_scoped_acquire gil;
    call(nb_trampoline, "_load", nb::bytes(in.data(), in.size()));
  }
};

auto refused(std::string const& peripheral, std::string expected) -> config_error
{
  return config_error("devices", 0, std::format("devices.{}", peripheral), std::move(expected));
}

auto spi_attachment_of(std::string const& peripheral, nb::object const& object)
    -> stm32::spi_attachment
{
  std::shared_ptr<stm32::spi_target> target;
  if(not nb::try_cast(object, target) or not target)
  {
    throw refused(peripheral, "popcorn.board.SpiTarget instances");
  }
  auto const pin = nb::cast<std::optional<std::uint8_t>>(object.attr("_cs_pin"));
  return {
      .device  = std::move(target),
      .cs_port = nb::cast<std::string>(object.attr("_cs_port")),
      .cs_pin  = pin};
}
}

void bind_device_bases(nb::module_ scope)
{
  nb::class_<stm32::i2c_target, python_i2c_target>(
      scope, "I2CTargetBase",
      "Native side of popcorn.board.I2CTarget: an I2C target implemented in Python.")
      .def(nb::init<>());
  nb::class_<stm32::spi_target, python_spi_target>(
      scope, "SpiTargetBase",
      "Native side of popcorn.board.SpiTarget: an SPI device implemented in Python.")
      .def(nb::init<>());
}

auto device_factories(
    mcu_description const&                                description,
    std::map<std::string, std::vector<nb::object>> const& devices)
    -> std::map<std::string, periph::model_factory, std::less<>>
{
  stm32::board board;
  for(auto const& [peripheral, objects] : devices)
  {
    auto const unit = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
      return p.name == peripheral and not p.alias_of;
    });
    if(unit == description.peripherals.end() or (unit->ip != "I2C" and unit->ip != "SPI"))
    {
      throw refused(peripheral, "an I2C or SPI peripheral of the MCU");
    }
    for(auto const& object : objects)
    {
      if(std::shared_ptr<stm32::i2c_target> target; nb::try_cast(object, target) and target)
      {
        if(unit->ip != "I2C")
        {
          throw refused(peripheral, "an SPI peripheral for an SpiTarget, not an I2CTarget");
        }
        board.i2c[peripheral].push_back(std::move(target));
      }
      else
      {
        if(unit->ip != "SPI")
        {
          throw refused(peripheral, "an I2C peripheral for an I2CTarget");
        }
        board.spi[peripheral].push_back(spi_attachment_of(peripheral, object));
      }
    }
  }
  auto all = stm32::factories(board, description);
  std::map<std::string, periph::model_factory, std::less<>> out;
  for(auto const& [peripheral, objects] : devices)
  {
    if(objects.empty())
    {
      continue;
    }
    auto found = all.find(peripheral);
    if(found == all.end())
    {
      throw refused(peripheral, "a peripheral a built-in I2C or SPI model serves");
    }
    out.emplace(peripheral, std::move(found->second));
  }
  return out;
}
}
