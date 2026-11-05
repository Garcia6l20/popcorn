#include "devices.hpp"
#include "chip_select.hpp"
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

/**
 * @brief The periph::i2c_target calls of a Python I2C target, each taking the GIL and calling
 *        `Self::invoke`, the hook lookup of the trampoline or of one attachment.
 */
template <typename Self> class i2c_target_calls : public periph::i2c_target
{
public:
  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    gil_entry const gil;
    return or_value<std::uint8_t>(self().invoke("_address"), no_address);
  }

  [[nodiscard]] auto start(bool read, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(self().invoke("_start", read, at.ps));
  }

  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> periph::i2c_reply override
  {
    gil_entry const gil;
    return reply_of(self().invoke("_write", byte, at.ps));
  }

  [[nodiscard]] auto read(vtime at) -> periph::i2c_read_reply override
  {
    gil_entry const gil;
    return read_reply_of(self().invoke("_read", at.ps));
  }

  void stop(vtime at) override
  {
    gil_entry const gil;
    self().invoke("_stop", at.ps);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    self().invoke("_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(self().invoke("_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    self().invoke("_load", nb::bytes(in.data(), in.size()));
  }

private:
  [[nodiscard]] auto self() const -> Self const&
  {
    return static_cast<Self const&>(*this);
  }
};

/**
 * @brief nanobind trampoline letting a Python class implement periph::i2c_target through the
 *        private methods `_address`, `_start`, `_write`, `_read`, `_stop`, `_reset`, `_save` and
 *        `_load` that `popcorn.board.I2CTarget` defines, which check the values returned:
 *        `_start` and `_write` return (ack, error, stretch_ps), `_read` (byte, error,
 *        stretch_ps), error an i2c_error value. Times cross as picoseconds since power-on. The
 *        engine calls it only through an attachment.
 */
class python_i2c_target final : public i2c_target_calls<python_i2c_target>
{
public:
  NB_TRAMPOLINE(i2c_target_calls<python_i2c_target>, 8);

  /**
   * @brief Calls nothing: the engine reaches the device only through an attachment, see adopt().
   * @return Empty.
   */
  template <typename... Args> auto invoke(char const*, Args&&...) const -> std::optional<nb::object>
  {
    return std::nullopt;
  }
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
 * @brief The periph::spi_target calls of a Python SPI device, each taking the GIL and calling
 *        `Self::invoke`, the hook lookup of the trampoline or of one attachment.
 */
template <typename Self> class spi_target_calls : public periph::spi_target
{
public:
  void select(bool selected, vtime at) override
  {
    gil_entry const gil;
    self().invoke("_select", selected, at.ps);
  }

  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override
  {
    gil_entry const gil;
    return or_value<std::uint32_t>(self().invoke("_exchange", out, bits, at.ps), 0);
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    self().invoke("_reset", kind);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    append(self().invoke("_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    self().invoke("_load", nb::bytes(in.data(), in.size()));
  }

private:
  [[nodiscard]] auto self() const -> Self const&
  {
    return static_cast<Self const&>(*this);
  }
};

/**
 * @brief nanobind trampoline letting a Python class implement periph::spi_target through the
 *        private methods `_select`, `_exchange`, `_reset`, `_save` and `_load` that
 *        `popcorn.board.SpiTarget` defines, which check the values returned. mode() is the
 *        SPI mode given at construction. Times cross as picoseconds since power-on. The engine
 *        calls it only through an attachment.
 */
class python_spi_target final : public spi_target_calls<python_spi_target>
{
public:
  NB_TRAMPOLINE(spi_target_calls<python_spi_target>, 5);

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

  /**
   * @brief Calls nothing: the engine reaches the device only through an attachment, see adopt().
   * @return Empty.
   */
  template <typename... Args> auto invoke(char const*, Args&&...) const -> std::optional<nb::object>
  {
    return std::nullopt;
  }

private:
  std::optional<std::uint8_t> declared_;
};

/**
 * @brief The periph::spi_controller_sink call of a Python SPI controller, taking the GIL and
 *        calling `Self::invoke`, the hook lookup of the trampoline or of one attachment.
 */
template <typename Self> class spi_controller_calls : public periph::spi_controller_sink
{
public:
  void frame(std::uint32_t sent, std::uint32_t received, vtime at) override
  {
    gil_entry const gil;
    self().invoke("_frame", sent, received, at.ps);
  }

private:
  [[nodiscard]] auto self() const -> Self const&
  {
    return static_cast<Self const&>(*this);
  }
};

/**
 * @brief nanobind trampoline letting a Python class implement periph::spi_controller_sink through
 *        the private method `_frame` that `popcorn.board.SpiController` defines, called once per
 *        frame. mode() is the SPI mode given at construction. Times cross as picoseconds since
 *        reset. The engine calls it only through an attachment.
 */
class python_spi_controller final : public spi_controller_calls<python_spi_controller>
{
public:
  NB_TRAMPOLINE(spi_controller_calls<python_spi_controller>, 1);

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

  /**
   * @brief Calls nothing: the engine reaches the device only through an attachment, see adopt().
   * @return Empty.
   */
  template <typename... Args> auto invoke(char const*, Args&&...) const -> std::optional<nb::object>
  {
    return std::nullopt;
  }

private:
  std::optional<std::uint8_t> declared_;
};

/**
 * @brief The periph::i2c_controller_sink calls of a Python I2C controller: both call
 *        `_answered` through `Self::invoke`, the hook lookup of the trampoline or of one
 *        attachment, the GIL taken.
 */
template <typename Self> class i2c_controller_calls : public periph::i2c_controller_sink
{
public:
  void answered(std::uint64_t unit, periph::i2c_phase phase, periph::i2c_answer reply, vtime at)
      override
  {
    answer(unit, phase, reply, at);
  }

  void dropped(std::uint64_t unit, periph::i2c_phase phase, vtime at) override
  {
    answer(unit, phase, {}, at);
  }

private:
  /**
   * @brief The phase answer `_answered` takes, the undriven one for a dropped phase: NACK for an
   *        address or a written byte, 0xFF with ack false for a read byte.
   */
  void answer(std::uint64_t unit, periph::i2c_phase phase, periph::i2c_answer reply, vtime at) const
  {
    gil_entry const gil;
    self().invoke(
        "_answered", unit, static_cast<std::uint8_t>(phase), reply.ack, reply.byte,
        static_cast<std::uint8_t>(reply.error), at.ps);
  }

  [[nodiscard]] auto self() const -> Self const&
  {
    return static_cast<Self const&>(*this);
  }
};

/**
 * @brief nanobind trampoline letting a Python class implement periph::i2c_controller_sink
 *        through the private method `_answered` that `popcorn.board.I2CController` defines. One
 *        hook name, so one trampoline slot: dropped() calls `_answered` with the undriven answer
 *        at its report time, one call per phase. Times cross as picoseconds since reset.
 *        The engine calls it only through an attachment.
 */
class python_i2c_controller final : public i2c_controller_calls<python_i2c_controller>
{
public:
  NB_TRAMPOLINE(i2c_controller_calls<python_i2c_controller>, 1);

  /**
   * @brief Calls nothing: the engine reaches the device only through an attachment, see adopt().
   * @return Empty.
   */
  template <typename... Args> auto invoke(char const*, Args&&...) const -> std::optional<nb::object>
  {
    return std::nullopt;
  }
};

/**
 * @brief One machine's attachment of a Python device: the machine's engine calls it, and it
 *        calls the device in its own cell. A device reattached to another machine gets a new
 *        attachment, so an old engine still running never calls it in the new machine's
 *        context, and is refused once its own machine is gone. Every hook call takes the GIL,
 *        mode() takes none. Once its machine's Python side is collected no hook is called and
 *        the run is asked to stop: an I2C target's address matches nothing, `_start` and
 *        `_write` NACK, a read returns 0, an SPI exchange returns 0.
 */
template <typename Device> class attachment
{
public:
  /** @brief Attaches @p device. */
  explicit attachment(std::shared_ptr<Device> device) noexcept : device_{std::move(device)}
  {}

  /**
   * @brief Calls the device's Python method @p name in the cell of this attachment.
   * @return Its result, empty when the cell refused the call.
   */
  template <typename... Args>
  auto invoke(char const* name, Args&&... args) const -> std::optional<nb::object>
  {
    return call(device_->nb_trampoline, cell, name, std::forward<Args>(args)...);
  }

  /** @brief The cell of this attachment. */
  callback_cell cell;

protected:
  /** @brief The device attached. */
  std::shared_ptr<Device> device_;
};

/** @brief One machine's attachment of a Python I2C target. */
class attached_i2c_target final : public i2c_target_calls<attached_i2c_target>,
                                  public attachment<python_i2c_target>
{
public:
  using attachment::attachment;
};

/** @brief One machine's attachment of a Python SPI target. */
class attached_spi_target final : public spi_target_calls<attached_spi_target>,
                                  public attachment<python_spi_target>
{
public:
  using attachment::attachment;

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return device_->mode();
  }
};

/** @brief One machine's attachment of a Python SPI controller. */
class attached_spi_controller final : public spi_controller_calls<attached_spi_controller>,
                                      public attachment<python_spi_controller>
{
public:
  using attachment::attachment;

  [[nodiscard]] auto mode() const -> std::optional<std::uint8_t> override
  {
    return device_->mode();
  }
};

/** @brief One machine's attachment of a Python I2C controller. */
class attached_i2c_controller final : public i2c_controller_calls<attached_i2c_controller>,
                                      public attachment<python_i2c_controller>
{
public:
  using attachment::attachment;
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
  nb::class_<periph::i2c_controller_sink, python_i2c_controller>(
      scope, "I2CControllerBase",
      "Native side of popcorn.board.I2CController: an I2C controller implemented in Python.")
      .def(nb::init<>());
}

auto prepare_devices(
    part_description const&                               description,
    std::map<std::string, std::vector<nb::object>> const& devices,
    std::optional<stm32::rcc_options> const&              rcc,
    device_inputs                                         inputs,
    stm32::board&                                         board,
    std::vector<nb::handle>&                              python_owned) -> device_plan
{
  device_cells cells;
  if(rcc)
  {
    board.rcc = *rcc;
  }
  std::vector<std::pair<std::string, std::string_view>> served;
  for(auto const& [peripheral, objects] : devices)
  {
    auto const unit = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
      return p.name == peripheral and not p.alias_of;
    });
    if(unit == description.peripherals.end() or (unit->ip != "I2C" and unit->ip != "SPI"))
    {
      throw refused(peripheral, "an I2C or SPI peripheral of the MCU");
    }
    bool const i2c_bus = unit->ip == "I2C";
    bool const spi_bus = unit->ip == "SPI";
    if(not objects.empty())
    {
      served.emplace_back(peripheral, i2c_bus ? "i2c" : "spi");
    }
    for(auto const& object : objects)
    {
      if(std::shared_ptr<periph::i2c_target> target; nb::try_cast(object, target) and target)
      {
        adopt<python_i2c_target, attached_i2c_target>(
            target, object, peripheral, cells, python_owned);
        if(not i2c_bus)
        {
          throw refused(peripheral, "an SPI peripheral for an SpiTarget, not an I2CTarget");
        }
        if(board.i2c_controllers.contains(peripheral))
        {
          throw refused(peripheral, "one I2CController alone on the I2C instance");
        }
        board.i2c[peripheral].push_back(std::move(target));
      }
      else if(
          std::shared_ptr<periph::i2c_controller_sink> controller;
          nb::try_cast(object, controller) and controller)
      {
        if(not i2c_bus)
        {
          throw refused(peripheral, "an I2C peripheral for an I2CController");
        }
        if(board.i2c_controllers.contains(peripheral) or board.i2c.contains(peripheral))
        {
          throw refused(peripheral, "one I2CController alone on the I2C instance");
        }
        adopt<python_i2c_controller, attached_i2c_controller>(
            controller, object, peripheral, cells, python_owned);
        board.i2c_controllers[peripheral] = std::move(controller);
      }
      else if(
          std::shared_ptr<periph::spi_controller_sink> controller;
          nb::try_cast(object, controller) and controller)
      {
        if(not spi_bus)
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
        if(not spi_bus)
        {
          throw refused(peripheral, "an I2C peripheral for an I2CTarget");
        }
        board.spi[peripheral].push_back(
            spi_attachment_of(description, peripheral, object, cells, python_owned));
      }
    }
  }
  auto const& known = inputs();
  auto const  reads = [&](std::string const& peripheral, std::string_view input) {
    auto const found = known.find(peripheral);
    return found != known.end() and std::ranges::contains(found->second, input);
  };
  device_plan plan;
  for(auto const& [peripheral, member] : served)
  {
    if(not reads(peripheral, member))
    {
      throw refused(peripheral, "a peripheral a built-in I2C or SPI model serves");
    }
    plan.names.push_back(peripheral);
  }
  if(rcc)
  {
    auto const unit = std::ranges::find_if(description.peripherals, [&](peripheral_desc const& p) {
      return not p.alias_of and reads(p.name, "rcc");
    });
    if(unit == description.peripherals.end())
    {
      throw config_error("rcc", 0, "rcc", "an MCU whose RCC a built-in model serves");
    }
    plan.names.push_back(unit->name);
  }
  std::ranges::sort(plan.names);
  plan.cells = std::move(cells);
  return plan;
}

auto device_factories(
    device_plan&                                               plan,
    std::map<std::string, periph::model_factory, std::less<>>& all)
    -> std::map<std::string, periph::model_factory, std::less<>>
{
  std::map<std::string, periph::model_factory, std::less<>> out;
  for(auto const& name : plan.names)
  {
    auto& made  = all.at(name);
    auto  cells = plan.cells.find(name);
    out.emplace(
        name, cells == plan.cells.end() ? std::move(made)
                                        : stopping(std::move(made), std::move(cells->second)));
  }
  return out;
}
}
