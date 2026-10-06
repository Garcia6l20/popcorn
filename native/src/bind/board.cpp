#include "board.hpp"
#include "gil_locks.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind
{
uart_link::uart_link(
    periph::stm32::uart_port& port,
    detail::alive_flag        alive,
    detail::host_ref          host)
    : port_{&port}, endpoint_{std::make_shared<detail::queue_endpoint>()}, alive_{std::move(alive)},
      host_{std::move(host)}
{
  port.attach(endpoint_);
}

auto uart_link::port() const -> periph::stm32::uart_port&
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("uart", 0, "machine", "a UART whose machine still exists");
  }
  return *port_;
}

void uart_link::send(reflex::py::nb::bytes const& data)
{
  auto const bytes = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  std::vector<std::uint16_t> const frames(bytes.begin(), bytes.end());
  auto&                            uart = port();
  detail::host_access(host_, [&] { uart.receive(frames); });
}

auto uart_link::queued() const -> std::uint64_t
{
  auto& uart = port();
  return detail::host_access(host_, [&] { return uart.queued(); });
}

auto uart_link::available() const -> std::uint64_t
{
  return detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { return endpoint_->available(held); });
}

auto uart_link::drain(std::uint64_t count) -> std::vector<uart_frame>
{
  auto const taken = detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { return endpoint_->take(held, count); });
  return taken
       | std::views::transform([](detail::queued_uart_frame const& frame) {
           return uart_frame{.data = frame.data, .at_ps = frame.at_ps};
         })
       | std::ranges::to<std::vector>();
}

auto uart_link::dropped() const -> std::uint64_t
{
  return detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { return endpoint_->dropped(held); });
}

void uart_link::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("uart", 0, "limit", "a frame limit of at least 1");
  }
  detail::link_locked(
      *endpoint_, [&](core::link_guard const& held) { endpoint_->set_limit(held, limit); });
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
  auto& queue = endpoint();
  auto  taken = detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.take(held, count); });
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
  for(auto const& unit : engine_->description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::audio_port*>(engine_->model(unit.name)) != nullptr)
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
  auto* const port = dynamic_cast<periph::stm32::audio_port*>(engine_->model(peripheral));
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
  for(auto const& unit : engine_->description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::uart_port*>(engine_->model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_uart(std::string const& peripheral) -> uart_link
{
  auto* const port = dynamic_cast<periph::stm32::uart_port*>(engine_->model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return access([&](core::machine&) { return uart_link{*port, alive_, slot_}; });
}

auto machine_handle::gpio(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::gpio_port&
{
  auto* const port = dynamic_cast<periph::stm32::gpio_port*>(engine_->model(peripheral));
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

void machine_handle::gpio_drive(
    std::string const&  peripheral,
    std::uint8_t        pin,
    std::optional<bool> level)
{
  access([&](core::machine&) { gpio(peripheral, pin).drive(pin, level); });
}

auto machine_handle::gpio_level(std::string const& peripheral, std::uint8_t pin) const -> bool
{
  return access([&](core::machine&) { return gpio(peripheral, pin).level(pin); });
}

auto machine_handle::gpio_mode(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::pin_mode
{
  return access([&](core::machine&) { return gpio(peripheral, pin).mode(pin); });
}

auto machine_handle::analog(std::string const& peripheral) const -> periph::stm32::analog_port&
{
  auto* const port = dynamic_cast<periph::stm32::analog_port*>(engine_->model(peripheral));
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
  access([&](core::machine&) { analog(peripheral).drive(channel, microvolts); });
}

auto machine_handle::adc_input(std::string const& peripheral, std::uint8_t channel) const
    -> std::uint32_t
{
  return access([&](core::machine&) { return analog(peripheral).input(channel); });
}

auto machine_handle::gpio_alternate(std::string const& peripheral, std::uint8_t pin) const
    -> std::uint8_t
{
  return access([&](core::machine&) { return gpio(peripheral, pin).alternate(pin); });
}
}
