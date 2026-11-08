#include "devices.hpp"
#include "chip_select.hpp"
#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/spi.hpp>
#include <popcorn/periph/uart.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>
#include <nanobind/trampoline.h>

#include <algorithm>
#include <atomic>
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
 * @brief nanobind trampoline letting a Python class implement periph::uart_endpoint through the
 *        private methods of `popcorn.board.UartDevice`: `_attached`, `_frame`, `_break`,
 *        `_aborted`, `_received`, `_reset`, `_rts`, `_error`, `_save` and `_load`, ten names, the
 *        trampoline's slot count. Times cross as picoseconds since the last reset. The engine
 *        calls it only through an attachment.
 */
class python_uart_device final : public periph::uart_endpoint
{
public:
  NB_TRAMPOLINE(periph::uart_endpoint, 10);

  /**
   * @brief A device hearing RTS edges or not, fixed for its life.
   * @param rts Whether its class overrides `on_rts`, so an edge costs no GIL entry otherwise.
   */
  explicit python_uart_device(bool rts) : rts_{rts}
  {}

  void transmitted(std::uint16_t, vtime, bool) override
  {}

  [[nodiscard]] auto hears_rts() const -> bool override
  {
    return rts_;
  }

  /** @brief Whether the device records the frames it is sent, see set_tapping(). */
  [[nodiscard]] auto tapping() const -> bool
  {
    return tapping_.load(std::memory_order_relaxed);
  }

  /** @brief Turns the tap of frames queued towards the firmware on or off, from any thread. */
  void set_tapping(bool on)
  {
    tapping_.store(on, std::memory_order_relaxed);
  }

  /** @brief The format the device reads the firmware's frames in, see set_line(). */
  [[nodiscard]] auto line() const -> periph::line_format
  {
    auto const packed = line_.load(std::memory_order_relaxed);
    return {
        .baud       = static_cast<std::uint32_t>(packed),
        .parity     = static_cast<periph::line_parity>((packed >> 32) & 0xffu),
        .half_stops = static_cast<std::uint32_t>(packed >> 40)};
  }

  /** @brief Sets the format the device reads the next frame in, from any thread. */
  void set_line(periph::line_format format)
  {
    line_.store(
        std::uint64_t{format.baud}
            | std::uint64_t{static_cast<std::uint8_t>(format.parity)} << 32
            | std::uint64_t{format.half_stops} << 40,
        std::memory_order_relaxed);
  }

private:
  bool                       rts_;
  std::atomic<bool>          tapping_{false};
  std::atomic<std::uint64_t> line_{};
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

/**
 * @brief One machine's attachment of a Python UART device. Every hook call takes the GIL,
 *        awaiting_attach() and hears_rts() take none. attached() calls the device once for the
 *        attachment's life. Its snapshot bytes are whether that happened, then what `_save`
 *        returned, and load() hands both to `_load`, which only keeps them: the device's `restore`
 *        runs once the machine has finished its restore.
 */
class attached_uart_device final : public periph::uart_endpoint,
                                   public attachment<python_uart_device>
{
public:
  using attachment::attachment;

  void ending(periph::line_frame const& frame) override
  {
    ending_ = frame;
  }

  void transmitted(std::uint16_t data, vtime at, bool brk) override
  {
    gil_entry const gil;
    if(brk)
    {
      invoke("_break", at.ps, ending_.start.ps);
      return;
    }
    invoke("_frame", data, at.ps, ending_.start.ps, wire_data());
  }

  [[nodiscard]] auto heard_format() const -> periph::line_format override
  {
    return device_->line();
  }

  void line_error(periph::uart_error kind, vtime at) override
  {
    gil_entry const gil;
    invoke("_error", static_cast<std::uint32_t>(kind), at.ps, ending_.start.ps, wire_data());
  }

  void received(periph::line_frame const& frame) override
  {
    if(not device_->tapping())
    {
      return;
    }
    gil_entry const gil;
    invoke(
        "_received", frame.data, frame.start.ps, frame.end.ps,
        frame.kind == periph::frame_kind::brk);
  }

  [[nodiscard]] auto hears_rts() const -> bool override
  {
    return device_->hears_rts();
  }

  void rts_changed(bool ready, vtime at) override
  {
    gil_entry const gil;
    invoke("_rts", ready, at.ps);
  }

  void aborted(vtime at) override
  {
    auto const bits = ending_.word_bits - (ending_.parity == periph::line_parity::none ? 0u : 1u);
    auto const data = static_cast<std::uint16_t>(ending_.data & ((1u << bits) - 1u));
    gil_entry const gil;
    invoke("_aborted", at.ps, ending_.start.ps, data);
  }

  [[nodiscard]] auto aborts_by_event() const -> bool override
  {
    return true;
  }

  void reset(reset_kind kind) override
  {
    gil_entry const gil;
    invoke("_reset", kind);
  }

  [[nodiscard]] auto awaiting_attach() const -> bool override
  {
    return not attach_done_;
  }

  void attached(vtime at) override
  {
    attach_done_ = true;
    gil_entry const gil;
    invoke("_attached", at.ps);
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(attach_done_ ? std::byte{1} : std::byte{0});
    gil_entry const gil;
    append(invoke("_save"), out);
  }

  void load(std::span<std::byte const> in) override
  {
    attach_done_         = in.front() != std::byte{0};
    auto const      data = in.subspan(1);
    gil_entry const gil;
    invoke("_load", attach_done_, nb::bytes(data.data(), data.size()));
  }

private:
  [[nodiscard]] auto wire_data() const -> std::uint16_t
  {
    return periph::data_of(ending_.data, ending_.word_bits, ending_.parity);
  }

  bool               attach_done_{};
  periph::line_frame ending_{};
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
  nb::class_<periph::uart_endpoint, python_uart_device>(
      scope, "UartDeviceBase",
      "Native side of popcorn.board.UartDevice: a device on a UART implemented in Python.")
      .def(nb::init<bool>(), nb::arg("rts"))
      .def(
          "set_tapping",
          [](periph::uart_endpoint& device, bool on) {
            dynamic_cast<python_uart_device&>(device).set_tapping(on);
          },
          nb::arg("on"))
      .def(
          "set_line",
          [](periph::uart_endpoint& device, std::uint32_t baud, std::uint32_t parity,
             std::uint32_t half_stops) {
            dynamic_cast<python_uart_device&>(device).set_line(
                {.baud       = baud,
                 .parity     = static_cast<periph::line_parity>(parity),
                 .half_stops = half_stops});
          },
          nb::arg("baud"), nb::arg("parity"), nb::arg("half_stops"));
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
    bool const i2c_bus   = unit != description.peripherals.end() and unit->ip == "I2C";
    bool const spi_bus   = unit != description.peripherals.end() and unit->ip == "SPI";
    bool const uart_line = unit != description.peripherals.end()
                       and (unit->ip == "USART" or unit->ip == "UART" or unit->ip == "LPUART");
    if(not i2c_bus and not spi_bus and not uart_line)
    {
      throw refused(peripheral, "an I2C, SPI or UART peripheral of the MCU");
    }
    if(not objects.empty())
    {
      served.emplace_back(peripheral, i2c_bus ? "i2c" : spi_bus ? "spi" : "uarts");
    }
    for(auto const& object : objects)
    {
      if(std::shared_ptr<periph::uart_endpoint> device; nb::try_cast(object, device) and device)
      {
        if(not uart_line)
        {
          throw refused(peripheral, "a UART peripheral for a UartDevice");
        }
        if(board.uarts.contains(peripheral))
        {
          throw refused(peripheral, "one UartDevice alone on the UART");
        }
        adopt<python_uart_device, attached_uart_device>(
            device, object, peripheral, cells, python_owned);
        board.uarts[peripheral] = std::move(device);
      }
      else if(uart_line)
      {
        throw refused(peripheral, "a UartDevice on a UART peripheral");
      }
      else if(std::shared_ptr<periph::i2c_target> target; nb::try_cast(object, target) and target)
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
      throw refused(peripheral, "a peripheral a built-in I2C, SPI or UART model serves");
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
