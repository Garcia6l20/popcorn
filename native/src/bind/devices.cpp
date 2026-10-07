#include "devices.hpp"
#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/i2c.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>
#include <nanobind/trampoline.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
namespace
{
namespace nb    = reflex::py::nb;
namespace stm32 = periph::stm32;

template <typename Trampoline, typename... Args>
auto call(Trampoline const& self, nb::handle cell, char const* name, Args&&... args) -> nb::object
{
  cell_entry const   entered{cell};
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
    gil_entry const gil;
    return nb::cast<std::uint8_t>(call(nb_trampoline, context_, "_address"));
  }

  [[nodiscard]] auto start(bool read, vtime at) -> bool override
  {
    gil_entry const gil;
    return nb::cast<bool>(call(nb_trampoline, context_, "_start", read, at.ps));
  }

  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> bool override
  {
    gil_entry const gil;
    return nb::cast<bool>(call(nb_trampoline, context_, "_write", byte, at.ps));
  }

  [[nodiscard]] auto read(vtime at) -> std::uint8_t override
  {
    gil_entry const gil;
    return nb::cast<std::uint8_t>(call(nb_trampoline, context_, "_read", at.ps));
  }

  void stop(vtime at) override
  {
    gil_entry const gil;
    call(nb_trampoline, context_, "_stop", at.ps);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    call(nb_trampoline, context_, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(call(nb_trampoline, context_, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    call(nb_trampoline, context_, "_load", nb::bytes(in.data(), in.size()));
  }

  /**
   * @brief Sets the cell whose context every call runs in.
   * @param cell A weak reference to the cell, or None, the GIL held.
   */
  void run_in(nb::object cell)
  {
    context_ = std::move(cell);
  }

private:
  nb::object context_ = nb::none();
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
    gil_entry const gil;
    call(nb_trampoline, context_, "_select", selected, at.ps);
  }

  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override
  {
    gil_entry const gil;
    return nb::cast<std::uint32_t>(call(nb_trampoline, context_, "_exchange", out, bits, at.ps));
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    call(nb_trampoline, context_, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(call(nb_trampoline, context_, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    call(nb_trampoline, context_, "_load", nb::bytes(in.data(), in.size()));
  }

  /**
   * @brief Sets the cell whose context every call runs in.
   * @param cell A weak reference to the cell, or None, the GIL held.
   */
  void run_in(nb::object cell)
  {
    context_ = std::move(cell);
  }

private:
  nb::object context_ = nb::none();
};

auto bytes_of(std::span<std::byte const> data) -> nb::bytes
{
  return nb::bytes(data.data(), data.size());
}

auto vector_of(nb::bytes const& data) -> std::vector<std::byte>
{
  auto const first = static_cast<std::byte const*>(data.data());
  return {first, first + data.size()};
}

template <typename Device> auto saved(Device const& device) -> nb::bytes
{
  std::vector<std::byte> out;
  device.save(out);
  return bytes_of(out);
}

template <typename Device> void loaded(Device& device, nb::bytes const& data)
{
  device.load(vector_of(data));
}

auto nanoseconds(std::int64_t count) -> std::chrono::nanoseconds
{
  return std::chrono::nanoseconds{count};
}

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
  if(auto* const python = dynamic_cast<python_spi_target*>(target.get()))
  {
    python->run_in(building_context());
  }
  if(not nb::hasattr(object, "_cs_port") or not nb::hasattr(object, "_cs_pin"))
  {
    throw refused(peripheral, "popcorn.board.SpiTarget or popcorn.devices.At25sf instances");
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
    std::map<std::string, std::vector<nb::object>> const& devices,
    std::optional<stm32::rcc_options> const&              rcc)
    -> std::map<std::string, periph::model_factory, std::less<>>
{
  stm32::board board;
  if(rcc)
  {
    board.rcc = *rcc;
  }
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
        if(auto* const python = dynamic_cast<python_i2c_target*>(target.get()))
        {
          python->run_in(building_context());
        }
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
  if(rcc)
  {
    auto const unit  = std::ranges::find_if(description.peripherals, [](peripheral_desc const& p) {
      return p.ip == "RCC" and not p.alias_of;
    });
    auto       found = unit == description.peripherals.end() ? all.end() : all.find(unit->name);
    if(found == all.end())
    {
      throw config_error("rcc", 0, "rcc", "an MCU whose RCC a built-in model serves");
    }
    out.emplace(found->first, std::move(found->second));
  }
  return out;
}

void bind_native_devices(nb::module_ scope)
{
  namespace devices = periph::devices;
  nb::class_<devices::at25sf, stm32::spi_target>(
      scope, "At25sfBase", "Native side of popcorn.devices.At25sf: the native AT25SF flash.")
      .def(
          "__init__",
          [](devices::at25sf* self, std::uint8_t manufacturer, std::uint8_t memory_type,
             std::uint8_t capacity, std::uint8_t device_id, std::uint32_t size,
             std::string const& image, nb::bytes const& initial, std::int64_t page_program,
             std::int64_t erase_4k, std::int64_t erase_32k, std::int64_t erase_64k,
             std::int64_t chip_erase, std::int64_t write_status, std::int64_t power_down,
             std::int64_t release, std::int64_t reset) {
            new(self) devices::at25sf({
                .part =
                    {.manufacturer = manufacturer,
                           .memory_type  = memory_type,
                           .capacity     = capacity,
                           .device_id    = device_id,
                           .size         = size},
                .image        = image,
                .initial      = vector_of(initial),
                .page_program = nanoseconds(page_program),
                .erase_4k     = nanoseconds(erase_4k),
                .erase_32k    = nanoseconds(erase_32k),
                .erase_64k    = nanoseconds(erase_64k),
                .chip_erase   = nanoseconds(chip_erase),
                .write_status = nanoseconds(write_status),
                .power_down   = nanoseconds(power_down),
                .release      = nanoseconds(release),
                .reset        = nanoseconds(reset),
            });
          },
          nb::arg("manufacturer"), nb::arg("memory_type"), nb::arg("capacity"),
          nb::arg("device_id"), nb::arg("size"), nb::arg("image"), nb::arg("initial"),
          nb::arg("page_program"), nb::arg("erase_4k"), nb::arg("erase_32k"), nb::arg("erase_64k"),
          nb::arg("chip_erase"), nb::arg("write_status"), nb::arg("power_down"), nb::arg("release"),
          nb::arg("reset"))
      .def("_contents", [](devices::at25sf const& self) { return bytes_of(self.contents()); })
      .def("_snapshot", &saved<devices::at25sf>)
      .def("_restore", &loaded<devices::at25sf>, nb::arg("data"));

  nb::class_<devices::eeprom_24xx, stm32::i2c_target>(
      scope, "Eeprom24xxBase",
      "Native side of popcorn.devices.Eeprom24xx: the native 24xx I2C EEPROM.")
      .def(
          "__init__",
          [](devices::eeprom_24xx* self, std::uint8_t address, std::uint32_t size,
             std::uint8_t address_bytes, std::uint32_t page_size, std::int64_t write_cycle,
             std::string const& image) {
            new(self) devices::eeprom_24xx({
                .address       = address,
                .size          = size,
                .address_bytes = address_bytes,
                .page_size     = page_size,
                .write_cycle   = nanoseconds(write_cycle),
                .image         = image,
            });
          },
          nb::arg("address"), nb::arg("size"), nb::arg("address_bytes"), nb::arg("page_size"),
          nb::arg("write_cycle"), nb::arg("image"))
      .def("_contents", [](devices::eeprom_24xx const& self) { return bytes_of(self.contents()); })
      .def("_snapshot", &saved<devices::eeprom_24xx>)
      .def("_restore", &loaded<devices::eeprom_24xx>, nb::arg("data"));

  nb::class_<devices::table_target, stm32::i2c_target>(
      scope, "TableTargetBase",
      "Native side of popcorn.devices.TableTarget: the native table-scripted I2C target.")
      .def(
          "__init__",
          [](devices::table_target* self, std::uint8_t address, std::uint8_t register_bytes,
             std::map<std::uint32_t, std::vector<std::uint8_t>> replies, bool nack_unknown) {
            new(self) devices::table_target({
                .address        = address,
                .register_bytes = register_bytes,
                .replies        = std::move(replies),
                .nack_unknown   = nack_unknown,
            });
          },
          nb::arg("address"), nb::arg("register_bytes"), nb::arg("replies"),
          nb::arg("nack_unknown"))
      .def(
          "_writes",
          [](devices::table_target const& self) {
            std::vector<std::tuple<std::uint32_t, nb::bytes>> out;
            for(auto const& write : self.writes())
            {
              out.emplace_back(write.reg, nb::bytes(write.bytes.data(), write.bytes.size()));
            }
            return out;
          })
      .def("_snapshot", &saved<devices::table_target>)
      .def("_restore", &loaded<devices::table_target>, nb::arg("data"));
}
}
