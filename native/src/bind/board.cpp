#include "board.hpp"
#include "gil_locks.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/i2c.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind
{
uart_queue::uart_queue(
    periph::stm32::uart_port& port,
    detail::alive_flag        alive,
    detail::host_ref          host)
    : port_{&port}, endpoint_{std::make_shared<detail::queue_endpoint>()}, alive_{std::move(alive)},
      host_{std::move(host)}
{
  port.attach(endpoint_);
}

auto uart_queue::port() const -> periph::stm32::uart_port&
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("uart", 0, "machine", "a UART whose machine still exists");
  }
  return *port_;
}

namespace
{
auto frames_of(reflex::py::nb::bytes const& data) -> std::vector<std::uint16_t>
{
  auto const bytes = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  return {bytes.begin(), bytes.end()};
}
}

void uart_queue::send(reflex::py::nb::bytes const& data)
{
  auto const frames = frames_of(data);
  auto&      uart   = port();
  detail::host_access(host_, [&] { uart.receive(frames); });
}

auto uart_queue::command_send(reflex::py::nb::bytes const& data, std::uint8_t error) const
    -> access_op
{
  auto const tag = static_cast<periph::stm32::uart_error>(error);
  if(not is_enumerator(tag))
  {
    throw config_error("uart", 0, "error", "a UartError value");
  }
  std::ignore = port();
  auto link   = *this;
  link.host_.reset();
  auto frames = frames_of(data)
              | std::views::transform([tag](std::uint16_t word) {
                  return periph::stm32::host_frame{.data = word, .error = tag};
                })
              | std::ranges::to<std::vector>();
  return detail::link_op(
      [link = std::move(link), frames = std::move(frames)] { link.port().receive_frames(frames); });
}

auto uart_queue::command_break() const -> access_op
{
  std::ignore = port();
  auto link   = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link)] {
    std::array<periph::stm32::host_frame, 1> const frame{
        {{.kind = periph::stm32::frame_kind::brk}}};
    link.port().receive_frames(frame);
  });
}

namespace
{
template <typename Port>
auto live_port(Port* port, detail::alive_flag const& alive, char const* what) -> Port&
{
  if(not detail::is_alive(alive))
  {
    throw config_error(what, 0, "machine", "a link whose machine still exists");
  }
  return *port;
}
}

auto uart_queue::queued() const -> std::uint64_t
{
  auto& uart = port();
  return detail::host_access(host_, [&] { return uart.queued(); });
}

auto uart_queue::op_queued() const -> access_op
{
  return detail::link_op(
      [uart = &port(), alive = alive_] { return live_port(uart, alive, "uart").queued(); });
}

auto uart_queue::available() const -> std::uint64_t
{
  return detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { return endpoint_->available(held); });
}

auto uart_queue::peek(std::uint64_t start, std::uint64_t count) const -> reflex::py::nb::bytes
{
  auto const copied = detail::link_locked(*endpoint_, [&](core::link_guard const& held) {
    return endpoint_->peek(held, start, count);
  });
  return reflex::py::nb::bytes(copied.data(), copied.size());
}

auto uart_queue::drain(std::uint64_t count) -> std::vector<uart_frame>
{
  return drain_until(count, std::numeric_limits<std::uint64_t>::max());
}

auto uart_queue::drain_until(std::uint64_t count, std::uint64_t until_ps) -> std::vector<uart_frame>
{
  auto const taken = detail::link_locked(*endpoint_, [&](core::link_guard const& held) {
    return endpoint_->take(held, count, until_ps);
  });
  return taken
       | std::views::transform([](detail::queued_uart_frame const& frame) {
           return uart_frame{.data = frame.data, .at_ps = frame.at_ps, .brk = frame.brk};
         })
       | std::ranges::to<std::vector>();
}

auto uart_queue::dropped() const -> std::uint64_t
{
  return detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { return endpoint_->dropped(held); });
}

void uart_queue::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("uart", 0, "limit", "a frame limit of at least 1");
  }
  detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { endpoint_->set_limit(held, limit); });
}

auto uart_queue::bridged_link() const -> std::unique_ptr<detail::pty_link>
{
  return std::make_unique<detail::uart_pty_link>(port(), endpoint_);
}

auto uart_queue::native_port() const -> periph::stm32::uart_port&
{
  return port();
}

auto uart_queue::endpoint() const noexcept -> std::shared_ptr<detail::queue_endpoint> const&
{
  return endpoint_;
}

audio_link::audio_link(
    periph::stm32::audio_port& port,
    periph::stm32::audio_block block,
    std::uint32_t              decimation,
    std::uint32_t              external_frame_hz,
    bool                       clock_wired,
    detail::alive_flag         alive,
    detail::host_ref           host)
    : endpoint_{std::make_shared<detail::audio_queue_endpoint>()}, port_{&port}, block_{block},
      alive_{std::move(alive)}, host_{std::move(host)}
{
  port.attach(
      block, {.endpoint          = endpoint_,
              .decimation        = decimation,
              .external_frame_hz = external_frame_hz,
              .clock_wired       = clock_wired});
}

auto audio_link::endpoint() const -> detail::audio_queue_endpoint&
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("audio", 0, "machine", "an SAI whose machine still exists");
  }
  return *endpoint_;
}

auto audio_link::port() const -> periph::stm32::audio_port&
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("audio", 0, "machine", "an SAI whose machine still exists");
  }
  return *port_;
}

void audio_link::set_external_frame_hz(std::uint32_t frame_hz)
{
  auto& sai = port();
  detail::host_access(host_, [&] { sai.set_external_clock(block_, frame_hz); });
}

auto audio_link::external_frame_hz() const -> std::uint32_t
{
  auto& sai = port();
  return detail::host_access(host_, [&] { return sai.external_clock(block_); });
}

auto audio_link::op_external_frame_hz() const -> access_op
{
  return detail::link_op([sai = &port(), alive = alive_, block = block_] {
    return live_port(sai, alive, "audio").external_clock(block);
  });
}

auto audio_link::op_set_external_frame_hz(std::uint32_t frame_hz) const -> access_op
{
  return detail::link_op([sai = &port(), alive = alive_, block = block_, frame_hz] {
    live_port(sai, alive, "audio").set_external_clock(block, frame_hz);
  });
}

auto audio_link::available() const -> std::uint64_t
{
  auto& queue = endpoint();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.available(held); });
}

auto audio_link::dropped() const -> std::uint64_t
{
  auto& queue = endpoint();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.dropped(held); });
}

auto audio_link::queued() const -> std::uint64_t
{
  auto& queue = endpoint();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.queued(held); });
}

auto audio_link::drain(std::uint64_t count) -> std::vector<audio_record>
{
  return drain_until(count, std::numeric_limits<std::uint64_t>::max());
}

auto audio_link::drain_until(std::uint64_t count, std::uint64_t until_ps)
    -> std::vector<audio_record>
{
  auto& queue = endpoint();
  auto  taken = detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.take(held, count, until_ps); });
  return taken
       | std::views::as_rvalue
       | std::views::transform([](detail::queued_audio_frame&& frame) {
           return audio_record{
               .slots        = std::move(frame.slots),
               .data_bits    = frame.format.data_bits,
               .active_slots = frame.format.active_slots,
               .mono         = frame.format.mono,
               .at_ps        = frame.at_ps,
           };
         })
       | std::ranges::to<std::vector>();
}

void audio_link::write(std::vector<std::uint32_t> const& words)
{
  auto& queue = endpoint();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.write(held, words); });
}

auto audio_link::command_write(std::vector<std::uint32_t> const& words) const -> access_op
{
  std::ignore = endpoint();
  auto link   = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), words] {
    auto& queue = link.endpoint();
    detail::link_locked(queue, [&](core::link_guard const& held) { queue.write(held, words); });
  });
}

void audio_link::discard()
{
  auto& queue = endpoint();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.discard(held); });
}

void audio_link::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("audio", 0, "limit", "a frame limit of at least 1");
  }
  auto& queue = endpoint();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.set_limit(held, limit); });
}

auto machine_handle::audio_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : engine().description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::audio_port*>(engine().model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_audio(
    std::string const& peripheral,
    std::string const& block,
    std::uint32_t      decimation,
    std::uint32_t      external_frame_hz,
    bool               clock_wired) -> audio_link
{
  auto* const port = dynamic_cast<periph::stm32::audio_port*>(engine().model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  if(block != "A" and block != "B")
  {
    throw config_error(peripheral, 0, "block", std::format("an audio block A or B, not {}", block));
  }
  auto const which = block == "A" ? periph::stm32::audio_block::a : periph::stm32::audio_block::b;
  return access([&](core::machine&) {
    return audio_link{*port, which, decimation, external_frame_hz, clock_wired, alive_, slot_};
  });
}

auto machine_handle::uart_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : engine().description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::uart_port*>(engine().model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_uart(std::string const& peripheral) -> uart_queue
{
  auto* const port = dynamic_cast<periph::stm32::uart_port*>(engine().model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return access([&](core::machine&) { return uart_queue{*port, alive_, slot_}; });
}

auto machine_handle::gpio(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::gpio_port&
{
  auto* const port = dynamic_cast<periph::stm32::gpio_port*>(engine().model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  if(pin > 15)
  {
    throw config_error(peripheral, 0, "pin", std::format("a pin from 0 to 15, not {}", pin));
  }
  return *port;
}

namespace
{
auto gpio_drive_body(periph::stm32::gpio_port& port, std::uint8_t pin, std::optional<bool> level)
{
  return [port = &port, pin, level](core::machine&) { port->drive(pin, level); };
}

auto gpio_level_body(periph::stm32::gpio_port& port, std::uint8_t pin)
{
  return [port = &port, pin](core::machine&) { return port->level(pin); };
}

auto gpio_mode_body(periph::stm32::gpio_port& port, std::uint8_t pin)
{
  return [port = &port, pin](core::machine&) { return port->mode(pin); };
}

auto gpio_alternate_body(periph::stm32::gpio_port& port, std::uint8_t pin)
{
  return [port = &port, pin](core::machine&) { return port->alternate(pin); };
}

auto adc_drive_body(
    periph::stm32::analog_port& port,
    std::uint8_t                channel,
    std::uint32_t               microvolts)
{
  return [port = &port, channel, microvolts](core::machine&) { port->drive(channel, microvolts); };
}

auto adc_input_body(periph::stm32::analog_port& port, std::uint8_t channel)
{
  return [port = &port, channel](core::machine&) { return port->input(channel); };
}
}

void machine_handle::gpio_drive(
    std::string const&  peripheral,
    std::uint8_t        pin,
    std::optional<bool> level)
{
  access(
      [&](core::machine& target) { gpio_drive_body(gpio(peripheral, pin), pin, level)(target); });
}

auto machine_handle::gpio_level(std::string const& peripheral, std::uint8_t pin) const -> bool
{
  return access(
      [&](core::machine& target) { return gpio_level_body(gpio(peripheral, pin), pin)(target); });
}

auto machine_handle::gpio_mode(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::pin_mode
{
  return access(
      [&](core::machine& target) { return gpio_mode_body(gpio(peripheral, pin), pin)(target); });
}

auto machine_handle::analog(std::string const& peripheral) const -> periph::stm32::analog_port&
{
  auto* const port = dynamic_cast<periph::stm32::analog_port*>(engine().model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return *port;
}

void machine_handle::adc_drive(
    std::string const& peripheral,
    std::uint8_t       channel,
    std::uint32_t      microvolts)
{
  access([&](core::machine& target) {
    adc_drive_body(analog(peripheral), channel, microvolts)(target);
  });
}

auto machine_handle::adc_input(std::string const& peripheral, std::uint8_t channel) const
    -> std::uint32_t
{
  return access(
      [&](core::machine& target) { return adc_input_body(analog(peripheral), channel)(target); });
}

auto machine_handle::gpio_alternate(std::string const& peripheral, std::uint8_t pin) const
    -> std::uint8_t
{
  return access([&](core::machine& target) {
    return gpio_alternate_body(gpio(peripheral, pin), pin)(target);
  });
}

auto machine_handle::op_gpio_level(std::string const& peripheral, std::uint8_t pin) const
    -> access_op
{
  return detail::machine_op(gpio_level_body(gpio(peripheral, pin), pin));
}

auto machine_handle::op_gpio_drive(
    std::string const&  peripheral,
    std::uint8_t        pin,
    std::optional<bool> level) const -> access_op
{
  return detail::machine_op(gpio_drive_body(gpio(peripheral, pin), pin, level));
}

auto machine_handle::op_gpio_mode(std::string const& peripheral, std::uint8_t pin) const
    -> access_op
{
  return detail::machine_op(gpio_mode_body(gpio(peripheral, pin), pin));
}

auto machine_handle::op_gpio_alternate(std::string const& peripheral, std::uint8_t pin) const
    -> access_op
{
  return detail::machine_op(gpio_alternate_body(gpio(peripheral, pin), pin));
}

auto machine_handle::op_adc_input(std::string const& peripheral, std::uint8_t channel) const
    -> access_op
{
  return detail::machine_op(adc_input_body(analog(peripheral), channel));
}

auto machine_handle::command_i2c_hold(
    std::string const&           peripheral,
    std::optional<std::uint64_t> at_ps,
    std::uint64_t                for_ps) const -> access_op
{
  auto* const bus = dynamic_cast<periph::stm32::i2c_bus*>(engine().model(peripheral));
  if(bus == nullptr)
  {
    throw config_error(peripheral, 0, "devices", "a built-in I2C model");
  }
  auto constexpr last = std::numeric_limits<std::uint64_t>::max();
  if(for_ps == 0 or (at_ps and *at_ps > last - for_ps))
  {
    throw std::invalid_argument(
        "a bus hold lasts at least 1 ps and ends within the 64-bit picosecond range");
  }
  return detail::machine_op([bus, at_ps, for_ps](core::machine& target) {
    auto const now  = target.virtual_clock().time().ps;
    auto const from = at_ps.value_or(now);
    if(from < now)
    {
      throw std::invalid_argument(
          std::format("a bus hold at {} ps starts before the machine time {} ps", from, now));
    }
    if(from > last - for_ps)
    {
      throw std::invalid_argument("a bus hold ends within the 64-bit picosecond range");
    }
    bus->hold(vtime{from}, vtime{from + for_ps});
    return from;
  });
}

auto machine_handle::command_spi_transfer(
    std::string const&           peripheral,
    std::optional<std::uint64_t> at_ps,
    std::uint64_t                frame_ps,
    std::uint8_t                 bits,
    std::vector<std::uint32_t>   frames) const -> access_op
{
  auto* const port = dynamic_cast<periph::stm32::spi_controller_port*>(engine().model(peripheral));
  if(port == nullptr)
  {
    throw config_error(peripheral, 0, "devices", "a built-in SPI model");
  }
  return detail::machine_op(
      [port, at_ps, frame_ps, bits, frames = std::move(frames)](core::machine& target) {
        auto const from = at_ps.value_or(target.virtual_clock().time().ps);
        return port->transfer(vtime{from}, frame_ps, bits, frames).ps;
      });
}

auto machine_handle::op_adc_drive(
    std::string const& peripheral,
    std::uint8_t       channel,
    std::uint32_t      microvolts) const -> access_op
{
  return detail::machine_op(adc_drive_body(analog(peripheral), channel, microvolts));
}
auto uart_queue::predicate_available(std::uint64_t count) const -> native_predicate
{
  std::ignore = port();
  return native_predicate{
      [endpoint = endpoint_, alive = alive_, count] {
        return not detail::is_alive(alive)
            or detail::link_locked(*endpoint, [&](core::link_guard const& held) {
                 return endpoint->available(held) >= count;
               });
      },
      host_}
      .described("UART data");
}

auto uart_queue::predicate_changed(count_mark const& mark) const -> native_predicate
{
  std::ignore = port();
  return native_predicate{
      [endpoint = endpoint_, alive = alive_, mark = mark.shared()] {
        return not detail::is_alive(alive)
            or detail::link_locked(*endpoint, [&](core::link_guard const& held) {
                 return endpoint->available(held) != mark->load(std::memory_order_acquire);
               });
      },
      host_}
      .described("UART data");
}

auto audio_link::predicate_available(std::uint64_t count) const -> native_predicate
{
  std::ignore = endpoint();
  return native_predicate{
      [endpoint = endpoint_, alive = alive_, count] {
        return not detail::is_alive(alive)
            or detail::link_locked(*endpoint, [&](core::link_guard const& held) {
                 return endpoint->available(held) >= count;
               });
      },
      host_}
      .described("audio frames");
}
}
