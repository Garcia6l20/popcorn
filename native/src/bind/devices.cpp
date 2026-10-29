#include "devices.hpp"
#include "../periph/stm32/pin_range.hpp"
#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>
#include <nanobind/trampoline.h>

#include <algorithm>
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
  python_owned.push_back(object);
  auto device = std::dynamic_pointer_cast<Python>(target);
  if(not device)
  {
    return;
  }
  auto attached = std::make_shared<Attached>(std::move(device));
  attached->cell.run_in(building_context(), building_alive(), building_collected());
  cells[peripheral].push_back(std::shared_ptr<callback_cell>(attached, &attached->cell));
  target = std::move(attached);
}

auto stopping(periph::model_factory inner, std::vector<std::shared_ptr<callback_cell>> cells)
    -> periph::model_factory
{
  if(cells.empty())
  {
    return inner;
  }
  return [inner = std::move(inner), cells = std::move(cells)](
             periph::model_context& ctx, peripheral_desc const& desc) mutable {
    for(auto const& cell : cells)
    {
      cell->stop_through(ctx.services);
    }
    return inner(ctx, desc);
  };
}

void check_bonded_chip_select(
    part_description const&     description,
    std::string const&          port,
    std::optional<std::uint8_t> pin)
{
  auto const unit = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
    return p.name == port and not p.alias_of;
  });
  if(not pin or unit == description.peripherals.end() or not unit->bonded)
  {
    return;
  }
  if(*pin >= 16 or (*unit->bonded >> *pin & 1U) == 0)
  {
    throw config_error(
        port, 0, "pin", stm32::unbonded_pin_text(description.part, port, *pin, *unit->bonded));
  }
}

auto spi_attachment_of(
    part_description const&  description,
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
    throw refused(
        peripheral, "an SPI device with _cs_port and _cs_pin, as popcorn.board.SpiTarget, "
                    "popcorn.devices.At25sf or popcorn.devices.chip_select set them");
  }
  auto const pin  = nb::cast<std::optional<std::uint8_t>>(object.attr("_cs_pin"));
  auto       port = nb::cast<std::string>(object.attr("_cs_port"));
  check_bonded_chip_select(description, port, pin);
  return {.device = std::move(target), .cs_port = std::move(port), .cs_pin = pin};
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
        board.spi[peripheral].push_back(
            spi_attachment_of(description, peripheral, object, cells, python_owned));
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
}
