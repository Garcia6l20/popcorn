#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/plugin/bindings.hpp>

#include <nanobind/stl/map.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace nb      = nanobind;
namespace periph  = popcorn::periph;
namespace devices = popcorn::periph::devices;

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

void bind_devices(nb::module_ scope)
{
  nb::class_<devices::at25sf, periph::spi_target>(
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

  nb::class_<devices::eeprom_24xx, periph::i2c_target>(
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

  nb::class_<devices::table_target, periph::i2c_target>(
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

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
  bind_devices(m);
}
