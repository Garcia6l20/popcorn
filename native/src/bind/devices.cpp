#include "devices.hpp"
#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/at25sf.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
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
auto call(Trampoline const& self, callback_cell const& cell, char const* name, Args&&... args)
    -> std::optional<nb::object>
{
  return cell.call([&] {
    nb::detail::ticket ticket(self, name, true);
    return self.base().attr(ticket.key)(std::forward<Args>(args)...);
  });
}

constexpr std::uint8_t no_address = 0xFF;

template <typename T> auto or_value(std::optional<nb::object> const& result, T neutral) -> T
{
  return result ? nb::cast<T>(*result) : neutral;
}

auto reply_of(std::optional<nb::object> const& result) -> periph::i2c_reply
{
  if(not result)
  {
    return {.ack = false};
  }
  auto const [ack, error, stretch] =
      nb::cast<std::tuple<bool, std::uint8_t, std::uint64_t>>(*result);
  return {.ack = ack, .error = static_cast<periph::i2c_error>(error), .stretch_ps = stretch};
}

auto read_reply_of(std::optional<nb::object> const& result) -> periph::i2c_read_reply
{
  if(not result)
  {
    return {.byte = 0};
  }
  auto const [byte, error, stretch] =
      nb::cast<std::tuple<std::uint8_t, std::uint8_t, std::uint64_t>>(*result);
  return {.byte = byte, .error = static_cast<periph::i2c_error>(error), .stretch_ps = stretch};
}

void append(std::optional<nb::object> const& result, std::vector<std::byte>& out)
{
  if(not result)
  {
    return;
  }
  auto const bytes = nb::cast<nb::bytes>(*result);
  auto const data  = static_cast<std::byte const*>(bytes.data());
  out.insert(out.end(), data, data + bytes.size());
}

/**
 * @brief nanobind trampoline letting a Python class implement periph::i2c_target through the
 *        private methods `_address`, `_start`, `_write`, `_read`, `_stop`, `_reset`, `_save` and
 *        `_load` that `popcorn.board.I2CTarget` defines, which check the values returned:
 *        `_start` and `_write` return (ack, error, stretch_ps), `_read` (byte, error,
 *        stretch_ps), error an i2c_error value. Every call takes the GIL. Times cross as
 *        picoseconds since power-on. Once its machine's Python side is collected no method is
 *        called: the run is asked to stop, the address matches nothing, `_start` and `_write`
 *        NACK, a read returns 0.
 */
class python_i2c_target final : public periph::i2c_target
{
public:
  NB_TRAMPOLINE(periph::i2c_target, 8);

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    gil_entry const gil;
    return or_value<std::uint8_t>(call(nb_trampoline, cell, "_address"), no_address);
  }

  [[nodiscard]] auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(call(nb_trampoline, cell, "_start", read, at.ps));
  }

  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(call(nb_trampoline, cell, "_write", byte, at.ps));
  }

  [[nodiscard]] auto read(vtime at) -> periph::i2c_read_reply override
  {
    gil_entry const gil;
    return read_reply_of(call(nb_trampoline, cell, "_read", at.ps));
  }

  void stop(vtime at) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_stop", at.ps);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(call(nb_trampoline, cell, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_load", nb::bytes(in.data(), in.size()));
  }

  /**
   * @brief Calls the Python method @p name in @p through, the cell of one attachment.
   * @return Its result, empty when the cell refused the call.
   */
  template <typename... Args>
  auto via(callback_cell const& through, char const* name, Args&&... args) const
      -> std::optional<nb::object>
  {
    return call(nb_trampoline, through, name, std::forward<Args>(args)...);
  }

  /** @brief The cell its calls run in. */
  callback_cell cell;
};

auto spi_mode_of(nb::handle mode) -> std::optional<std::uint8_t>
{
  if(mode.is_none())
  {
    return std::nullopt;
  }
  return nb::cast<std::uint8_t>(mode);
}

/**
 * @brief nanobind trampoline letting a Python class implement periph::spi_target through the
 *        private methods `_select`, `_exchange`, `_reset`, `_save` and `_load` that
 *        `popcorn.board.SpiTarget` defines, which check the values returned. Every call takes
 *        the GIL, except mode(), the SPI mode given at construction. Times cross as picoseconds
 *        since power-on. Once its machine's Python side is collected no method is called: the
 *        run is asked to stop, an exchange returns 0.
 */
class python_spi_target final : public periph::spi_target
{
public:
  NB_TRAMPOLINE(periph::spi_target, 5);

  /** @brief A device declaring no SPI mode. */
  python_spi_target() = default;

  /**
   * @brief A device declaring SPI mode @p mode, fixed for its life.
   * @param mode CPOL << 1 | CPHA, 0 to 3, or None for no check.
   */
  explicit python_spi_target(nb::handle mode) : declared_{spi_mode_of(mode)}
  {}

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return declared_;
  }

  void select(bool selected, vtime at) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_select", selected, at.ps);
  }

  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override
  {
    gil_entry const gil;
    return or_value<std::uint32_t>(call(nb_trampoline, cell, "_exchange", out, bits, at.ps), 0);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(call(nb_trampoline, cell, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_load", nb::bytes(in.data(), in.size()));
  }

  /**
   * @brief Calls the Python method @p name in @p through, the cell of one attachment.
   * @return Its result, empty when the cell refused the call.
   */
  template <typename... Args>
  auto via(callback_cell const& through, char const* name, Args&&... args) const
      -> std::optional<nb::object>
  {
    return call(nb_trampoline, through, name, std::forward<Args>(args)...);
  }

  /** @brief The cell its calls run in. */
  callback_cell cell;

private:
  std::optional<std::uint8_t> declared_;
};

/**
 * @brief nanobind trampoline letting a Python class implement periph::spi_controller_sink through
 *        the private method `_frame` that `popcorn.board.SpiController` defines. Every call takes
 *        the GIL, one per frame, except mode(), the SPI mode given at construction. Times
 *        cross as picoseconds since reset. Once its machine's Python side is collected no method
 *        is called: the run is asked to stop.
 */
class python_spi_controller final : public periph::spi_controller_sink
{
public:
  NB_TRAMPOLINE(periph::spi_controller_sink, 1);

  /** @brief A controller declaring no SPI mode. */
  python_spi_controller() = default;

  /**
   * @brief A controller declaring SPI mode @p mode, fixed for its life.
   * @param mode CPOL << 1 | CPHA, 0 to 3, or None for no check.
   */
  explicit python_spi_controller(nb::handle mode) : declared_{spi_mode_of(mode)}
  {}

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return declared_;
  }

  void frame(std::uint32_t sent, std::uint32_t received, vtime at) override
  {
    gil_entry const gil;
    call(nb_trampoline, cell, "_frame", sent, received, at.ps);
  }

  /**
   * @brief Calls the Python method @p name in @p through, the cell of one attachment.
   * @return Its result, empty when the cell refused the call.
   */
  template <typename... Args>
  auto via(callback_cell const& through, char const* name, Args&&... args) const
      -> std::optional<nb::object>
  {
    return call(nb_trampoline, through, name, std::forward<Args>(args)...);
  }

  /** @brief The cell its calls run in. */
  callback_cell cell;

private:
  std::optional<std::uint8_t> declared_;
};

/**
 * @brief One machine's attachment of a Python I2C target: the machine's engine calls it, and
 *        it calls the device in its own cell. A device reattached to another machine gets a
 *        new attachment, so an old engine still running never calls it in the new machine's
 *        context, and is refused once its own machine is gone.
 */
class attached_i2c_target final : public periph::i2c_target
{
public:
  /** @brief Attaches @p device. */
  explicit attached_i2c_target(std::shared_ptr<python_i2c_target> device) noexcept
      : device_{std::move(device)}
  {}

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    gil_entry const gil;
    return or_value<std::uint8_t>(device_->via(cell, "_address"), no_address);
  }

  [[nodiscard]] auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(device_->via(cell, "_start", read, at.ps));
  }

  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(device_->via(cell, "_write", byte, at.ps));
  }

  [[nodiscard]] auto read(vtime at) -> periph::i2c_read_reply override
  {
    gil_entry const gil;
    return read_reply_of(device_->via(cell, "_read", at.ps));
  }

  void stop(vtime at) override
  {
    gil_entry const gil;
    device_->via(cell, "_stop", at.ps);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    device_->via(cell, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(device_->via(cell, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    device_->via(cell, "_load", nb::bytes(in.data(), in.size()));
  }

  /** @brief The cell of this attachment. */
  callback_cell cell;

private:
  std::shared_ptr<python_i2c_target> device_;
};

/** @brief One machine's attachment of a Python SPI target, as attached_i2c_target. */
class attached_spi_target final : public periph::spi_target
{
public:
  /** @brief Attaches @p device. */
  explicit attached_spi_target(std::shared_ptr<python_spi_target> device) noexcept
      : device_{std::move(device)}
  {}

  void select(bool selected, vtime at) override
  {
    gil_entry const gil;
    device_->via(cell, "_select", selected, at.ps);
  }

  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override
  {
    gil_entry const gil;
    return or_value<std::uint32_t>(device_->via(cell, "_exchange", out, bits, at.ps), 0);
  }

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return device_->mode();
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    device_->via(cell, "_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(device_->via(cell, "_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    device_->via(cell, "_load", nb::bytes(in.data(), in.size()));
  }

  /** @brief The cell of this attachment. */
  callback_cell cell;

private:
  std::shared_ptr<python_spi_target> device_;
};

/** @brief One machine's attachment of a Python SPI controller, as attached_i2c_target. */
class attached_spi_controller final : public periph::spi_controller_sink
{
public:
  /** @brief Attaches @p device. */
  explicit attached_spi_controller(std::shared_ptr<python_spi_controller> device) noexcept
      : device_{std::move(device)}
  {}

  void frame(std::uint32_t sent, std::uint32_t received, vtime at) override
  {
    gil_entry const gil;
    device_->via(cell, "_frame", sent, received, at.ps);
  }

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return device_->mode();
  }

  /** @brief The cell of this attachment. */
  callback_cell cell;

private:
  std::shared_ptr<python_spi_controller> device_;
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

using device_cells =
    std::map<std::string, std::vector<std::shared_ptr<callback_cell>>, std::less<>>;

template <typename Python, typename Attached, typename Target>
void adopt(
    std::shared_ptr<Target>& target,
    nb::object const&        object,
    std::string const&       peripheral,
    device_cells&            cells,
    std::vector<nb::handle>& python_owned)
{
  auto device = std::dynamic_pointer_cast<Python>(target);
  if(not device)
  {
    return;
  }
  auto attached = std::make_shared<Attached>(std::move(device));
  attached->cell.run_in(building_context(), building_alive(), building_collected());
  cells[peripheral].push_back(std::shared_ptr<callback_cell>(attached, &attached->cell));
  python_owned.push_back(object);
  target = std::move(attached);
}

auto stopping(periph::model_factory inner, std::vector<std::shared_ptr<callback_cell>> cells)
    -> periph::model_factory
{
  if(cells.empty())
  {
    return inner;
  }
  return [inner = std::move(inner),
          cells = std::move(cells)](periph::model_context& ctx, peripheral_desc const& desc) {
    for(auto const& cell : cells)
    {
      cell->stop_through(ctx.services);
    }
    return inner(ctx, desc);
  };
}

auto spi_attachment_of(
    std::string const&       peripheral,
    nb::object const&        object,
    device_cells&            cells,
    std::vector<nb::handle>& python_owned) -> stm32::spi_attachment
{
  std::shared_ptr<periph::spi_target> target;
  if(not nb::try_cast(object, target) or not target)
  {
    throw refused(peripheral, "popcorn.board.SpiTarget instances");
  }
  adopt<python_spi_target, attached_spi_target>(target, object, peripheral, cells, python_owned);
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
  nb::class_<periph::i2c_target, python_i2c_target>(
      scope, "I2CTargetBase",
      "Native side of popcorn.board.I2CTarget: an I2C target implemented in Python.")
      .def(nb::init<>());
  nb::class_<periph::spi_target, python_spi_target>(
      scope, "SpiTargetBase",
      "Native side of popcorn.board.SpiTarget: an SPI device implemented in Python.")
      .def(nb::init<nb::handle>(), nb::arg("mode") = nb::none());
  nb::class_<periph::spi_controller_sink, python_spi_controller>(
      scope, "SpiControllerBase",
      "Native side of popcorn.board.SpiController: an SPI controller implemented in Python.")
      .def(nb::init<nb::handle>(), nb::arg("mode") = nb::none());
}

auto device_factories(
    part_description const&                               description,
    std::map<std::string, std::vector<nb::object>> const& devices,
    std::optional<stm32::rcc_options> const&              rcc,
    std::vector<nb::handle>&                              python_owned)
    -> std::map<std::string, periph::model_factory, std::less<>>
{
  stm32::board board;
  device_cells cells;
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
      if(std::shared_ptr<periph::i2c_target> target; nb::try_cast(object, target) and target)
      {
        adopt<python_i2c_target, attached_i2c_target>(
            target, object, peripheral, cells, python_owned);
        if(unit->ip != "I2C")
        {
          throw refused(peripheral, "an SPI peripheral for an SpiTarget, not an I2CTarget");
        }
        board.i2c[peripheral].push_back(std::move(target));
      }
      else if(
          std::shared_ptr<periph::spi_controller_sink> controller;
          nb::try_cast(object, controller) and controller)
      {
        if(unit->ip != "SPI")
        {
          throw refused(peripheral, "an SPI peripheral for an SpiController");
        }
        if(board.spi_controllers.contains(peripheral) or board.spi.contains(peripheral))
        {
          throw refused(peripheral, "one SpiController alone on the SPI instance");
        }
        adopt<python_spi_controller, attached_spi_controller>(
            controller, object, peripheral, cells, python_owned);
        board.spi_controllers[peripheral] = std::move(controller);
      }
      else
      {
        if(board.spi_controllers.contains(peripheral))
        {
          throw refused(peripheral, "one SpiController alone on the SPI instance");
        }
        if(unit->ip != "SPI")
        {
          throw refused(peripheral, "an I2C peripheral for an I2CTarget");
        }
        board.spi[peripheral].push_back(spi_attachment_of(peripheral, object, cells, python_owned));
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
    out.emplace(peripheral, stopping(std::move(found->second), std::move(cells[peripheral])));
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
